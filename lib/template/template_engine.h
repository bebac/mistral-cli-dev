#ifndef MISTRAL_TEMPLATE_ENGINE_H
#define MISTRAL_TEMPLATE_ENGINE_H

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace mistral::tmpl
{
  // Simple template engine with three expansion types:
  //
  //   ${var}              — Variable substitution from a key-value map.
  //   ${{command}}        — Shell command expansion (captures stdout).
  //   {{> path}}          — File inclusion relative to an include base directory.
  //                         The .md extension is appended if missing.
  //                         Includes are resolved recursively with cycle detection.
  //   {{> path if=var}}   — Conditional include: only include if `var` is truthy.
  //   {{> path unless=var}} — Conditional include: only include if `var` is NOT truthy.
  //
  // Truthy values: "true", "1", "yes", "on" (case-insensitive).
  // Falsy values: "false", "0", "no", "off", "" (empty), or unset.
  //
  class template_engine
  {
  public:
    template_engine() = default;

    // Set the base directory for {{> path}} includes.
    void set_include_base(const std::filesystem::path& base_dir)
    {
      include_base_ = base_dir;
    }

    // Set a single variable.
    void set_variable(const std::string& name, const std::string& value)
    {
      variables_[name] = value;
    }

    // Set multiple variables at once.
    void set_variables(const std::map<std::string, std::string>& vars)
    {
      for (const auto& [k, v] : vars)
        variables_[k] = v;
    }

    // Enable/disable shell command expansion (enabled by default).
    void enable_command_expansion(bool enable)
    {
      command_expansion_ = enable;
    }

    // Process a template string. Returns the expanded result.
    std::string process(const std::string& input) const;

    // Load a file and process it as a template.
    std::string process_file(const std::filesystem::path& path) const;

  private:
    // Core processing with cycle detection for includes.
    std::string process_impl(const std::string& input, std::set<std::string>& include_stack) const;

    std::string expand_variable(const std::string& name) const;
    std::string expand_command(const std::string& command) const;
    std::string expand_include(
      const std::string& path,
      std::set<std::string>& include_stack,
      bool optional = false
    ) const;

    // Check if a variable is set to a truthy value.
    bool is_truthy(const std::string& name) const;

    static std::string read_file(const std::filesystem::path& path);

    std::filesystem::path include_base_;
    std::map<std::string, std::string> variables_;
    bool command_expansion_ = true;
  };

} // namespace mistral::tmpl

#endif // MISTRAL_TEMPLATE_ENGINE_H
