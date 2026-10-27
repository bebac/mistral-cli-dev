#include "agent/workspace_bootstrap.h"

#include "common/embedded_resources.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

using json = nlohmann::json;

namespace mistral::agent
{

  namespace
  {
    constexpr const char* k_managed_file = "sys_patchset_instructions.md";
    constexpr const char* k_manifest_file = "bootstrap.json";

    std::string read_file(const fs::path& path)
    {
      std::ifstream f(path, std::ios::in | std::ios::binary);
      if (!f)
      {
        throw std::runtime_error("cannot read file: " + path.string());
      }
      return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }

    void write_file(const fs::path& path, std::string_view content)
    {
      std::ofstream f(path, std::ios::out | std::ios::binary | std::ios::trunc);
      if (!f)
      {
        throw std::runtime_error("cannot write file: " + path.string());
      }
      f.write(content.data(), static_cast<std::streamsize>(content.size()));
      if (!f)
      {
        throw std::runtime_error("failed writing file: " + path.string());
      }
    }

    json load_manifest(const fs::path& path)
    {
      std::error_code ec;
      if (!fs::exists(path, ec))
      {
        return json::object();
      }
      try
      {
        std::ifstream f(path);
        json manifest;
        f >> manifest;
        if (!manifest.is_object())
        {
          return json::object();
        }
        return manifest;
      }
      catch (const std::exception&)
      {
        // Corrupt manifest: treat as absent. It is rewritten on the next
        // managed-file update.
        return json::object();
      }
    }

    json make_manifest_entry(std::string_view content)
    {
      return json{{"version", embedded::patchset_instructions_version}, {"hash", fnv1a_64_hex(content)}};
    }
  } // namespace

  std::string fnv1a_64_hex(std::string_view data)
  {
    constexpr std::uint64_t fnv_offset_basis = 0xcbf29ce484222325ULL;
    constexpr std::uint64_t fnv_prime = 0x100000001b3ULL;

    std::uint64_t hash = fnv_offset_basis;
    for (const char c : data)
    {
      hash ^= static_cast<unsigned char>(c);
      hash *= fnv_prime;
    }

    static constexpr char hex_digits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(16);
    for (int shift = 60; shift >= 0; shift -= 4)
    {
      hex.push_back(hex_digits[(hash >> shift) & 0xF]);
    }
    return hex;
  }

  bootstrap_result bootstrap_workspace(const fs::path& base_dir, const bootstrap_options& options)
  {
    bootstrap_result result;
    if (!options.enabled)
    {
      return result;
    }
    result.ran = true;

    std::error_code ec;
    fs::create_directories(base_dir, ec);

    // --- Seeded files: written only when missing, never overwritten. ---
    const std::vector<std::pair<const char*, std::string_view>> seeded_files = {
      {"mistral-cli.json", embedded::config_json()},
      {"policy.json", embedded::policy_json()},
      {"sys_prompt.md", embedded::sys_prompt()},
      {"sys_prompt_researcher.md", embedded::sys_prompt_researcher()},
      {"sys_prompt_xerj.md", embedded::sys_prompt_xerj()},
      {"sys_storybook_notes.md", embedded::sys_storybook_notes()},
    };

    for (const auto& [name, content] : seeded_files)
    {
      const fs::path path = base_dir / name;
      if (!fs::exists(path, ec))
      {
        write_file(path, content);
        result.created.push_back(name);
      }
    }

    // --- Managed file: updated from the embedded content when the on-disk
    // copy is an unmodified stock copy of an older version. ---
    const fs::path managed_path = base_dir / k_managed_file;
    const fs::path manifest_path = base_dir / k_manifest_file;
    json manifest = load_manifest(manifest_path);
    bool manifest_dirty = false;

    const std::string_view embedded_content = embedded::sys_patchset_instructions();

    if (!fs::exists(managed_path, ec))
    {
      write_file(managed_path, embedded_content);
      manifest["patchset_instructions"] = make_manifest_entry(embedded_content);
      manifest_dirty = true;
      result.created.push_back(k_managed_file);
    }
    else if (options.force_update)
    {
      write_file(managed_path, embedded_content);
      manifest["patchset_instructions"] = make_manifest_entry(embedded_content);
      manifest_dirty = true;
      result.updated.push_back(std::string(k_managed_file) + " (forced update)");
    }
    else
    {
      const auto entry_it = manifest.find("patchset_instructions");
      const bool tracked = entry_it != manifest.end() && entry_it->is_object();

      if (!tracked)
      {
        result.warnings.push_back(
          std::string(k_managed_file) + " exists but is not tracked by " + k_manifest_file + "; leaving it in place"
        );
      }
      else
      {
        const std::string recorded_hash = entry_it->value("hash", std::string());
        const int recorded_version = entry_it->value("version", 0);
        const std::string disk_hash = fnv1a_64_hex(read_file(managed_path));

        if (disk_hash == recorded_hash)
        {
          // Unmodified stock copy: update it if the executable is newer.
          if (recorded_version < embedded::patchset_instructions_version)
          {
            write_file(managed_path, embedded_content);
            manifest["patchset_instructions"] = make_manifest_entry(embedded_content);
            manifest_dirty = true;
            result.updated.push_back(
              std::string(k_managed_file) + " (v" + std::to_string(recorded_version) + " -> v" +
              std::to_string(embedded::patchset_instructions_version) + ")"
            );
          }
        }
        else
        {
          // Locally modified: never clobber without explicit consent.
          if (recorded_version < embedded::patchset_instructions_version)
          {
            result.warnings.push_back(
              std::string(k_managed_file) + " was modified locally; keeping it " +
              "(use --update-managed to overwrite with v" + std::to_string(embedded::patchset_instructions_version) +
              ")"
            );
          }
        }
      }
    }

    if (manifest_dirty)
    {
      write_file(manifest_path, manifest.dump(2) + "\n");
    }

    return result;
  }

} // namespace mistral::agent
