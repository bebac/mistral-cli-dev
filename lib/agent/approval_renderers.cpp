#include "agent/approval_renderers.h"

#include "agent/agent_system.h"
#include "agent/tool_registry.h"
#include "tools/edit_types.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mistral::agent
{
  namespace
  {
    // Maximum number of files listed in a patchset_apply approval prompt
    // before the list is truncated.
    constexpr size_t max_shown_files = 10;

    // Count lines in a text block. A trailing newline does not start a
    // new line.
    size_t line_count(std::string_view text)
    {
      if (text.empty())
        return 0;
      size_t count = 1;
      for (char c : text)
      {
        if (c == '\n')
          ++count;
      }
      if (text.back() == '\n')
        --count;
      return count;
    }

    std::string line_count_suffix(std::string_view text)
    {
      size_t count = line_count(text);
      return " (" + std::to_string(count) + (count == 1 ? " line)" : " lines)");
    }

    // All edits that target a single file, collapsed into one summary.
    struct file_summary_t
    {
      std::string file_path;
      size_t edit_count = 0;
      bool creates = false;    // any edit creates the file (search_text empty)
      bool overwrites = false; // any edit overwrites the file (search_text == ".")
      bool deletes = false;    // any edit deletes matched text (replace_text empty)
      bool modifies = false;   // any edit is a plain search/replace
      std::string line_suffix; // line count of the last create/overwrite edit
    };

    // Status letter for a file, derived from all of its edits:
    //   A = the file is created
    //   O = the file is overwritten as a whole
    //   D = every edit deletes matched text
    //   M = otherwise (search/replace)
    char status_letter(const file_summary_t& summary)
    {
      if (summary.creates)
        return 'A';
      if (summary.overwrites)
        return 'O';
      if (summary.deletes && !summary.modifies)
        return 'D';
      return 'M';
    }

    // Group edits by file path, keeping the order in which each file is
    // first touched.
    std::vector<file_summary_t> summarize_by_file(const std::vector<tools::file_edit_t>& edits)
    {
      std::vector<file_summary_t> summaries;
      std::unordered_map<std::string, size_t> index_by_path;

      for (const auto& edit : edits)
      {
        auto [it, inserted] = index_by_path.try_emplace(edit.file_path, summaries.size());
        if (inserted)
        {
          summaries.emplace_back();
          summaries.back().file_path = edit.file_path;
        }

        auto& summary = summaries[it->second];
        ++summary.edit_count;
        if (edit.search_text.empty())
        {
          summary.creates = true;
          summary.line_suffix = line_count_suffix(edit.replace_text);
        }
        else if (edit.search_text == ".")
        {
          summary.overwrites = true;
          summary.line_suffix = line_count_suffix(edit.replace_text);
        }
        else if (edit.replace_text.empty())
        {
          summary.deletes = true;
        }
        else
        {
          summary.modifies = true;
        }
      }
      return summaries;
    }

    // One line per file: file index, status letter, file path, and either
    // the line count (single create/overwrite) or the number of edits.
    std::string render_patchset_apply_approval(const json& args, std::string_view reason)
    {
      std::string patchset_id;
      if (args.contains("patchset_id") && args["patchset_id"].is_string())
      {
        patchset_id = args["patchset_id"].get<std::string>();
      }

      std::ostringstream oss;
      oss << "\n[APPROVAL REQUIRED] patchset_apply\n\n";

      auto patchset = agent_system::instance().get_patchset(patchset_id);
      if (!patchset)
      {
        oss << "  patchset: " << patchset_id << " (not found)\n";
      }
      else
      {
        const auto& edits = patchset->edits;
        auto files = summarize_by_file(edits);

        oss << "  patchset: " << patchset_id << " (" << edits.size() << (edits.size() == 1 ? " edit" : " edits");
        if (!files.empty())
        {
          oss << " in " << files.size() << (files.size() == 1 ? " file" : " files");
        }
        oss << ")\n";

        size_t shown = std::min(files.size(), max_shown_files);
        for (size_t i = 0; i < shown; ++i)
        {
          const auto& file = files[i];
          oss << "    " << (i + 1) << ". " << status_letter(file) << ' ' << file.file_path;
          if (file.edit_count > 1)
          {
            oss << " (" << file.edit_count << " edits)";
          }
          else
          {
            oss << file.line_suffix;
          }
          oss << "\n";
        }
        if (files.size() > max_shown_files)
        {
          oss << "    ... and " << (files.size() - max_shown_files) << " more files\n";
        }
      }

      oss << "\n  Reason: " << reason << "\n";
      oss << "  Approve? [y/n/r/a/?]: ";
      return oss.str();
    }

    std::string render_exec_approval(const json& args, std::string_view reason)
    {
      std::ostringstream cmd_line;
      if (args.contains("command") && args["command"].is_string())
      {
        cmd_line << args["command"].get<std::string>();
      }
      if (args.contains("args") && args["args"].is_array())
      {
        for (const auto& a : args["args"])
        {
          if (a.is_string())
          {
            cmd_line << ' ' << a.get<std::string>();
          }
        }
      }

      std::ostringstream oss;
      oss << "\n[APPROVAL REQUIRED] exec\n\n";
      oss << "  Command: " << cmd_line.str() << "\n";
      if (args.contains("cwd") && args["cwd"].is_string())
      {
        std::string cwd = args["cwd"].get<std::string>();
        if (!cwd.empty())
        {
          oss << "  Working directory: " << cwd << "\n";
        }
      }
      oss << "\n  Reason: " << reason << "\n";
      oss << "  Approve? [y/n/r/a/?]: ";
      return oss.str();
    }
  } // namespace

  void register_approval_renderers(tool_registry& tools)
  {
    tools.register_approval_renderer("patchset_apply", [](const json& args, std::string_view reason) {
      return render_patchset_apply_approval(args, reason);
    });
    tools.register_approval_renderer("exec", [](const json& args, std::string_view reason) {
      return render_exec_approval(args, reason);
    });
  }
} // namespace mistral::agent
