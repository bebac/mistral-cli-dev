#ifndef MISTRAL_POLICY_ENGINE_H
#define MISTRAL_POLICY_ENGINE_H

#include <filesystem>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace mistral::agent
{
  using json = nlohmann::ordered_json;

  namespace fs = std::filesystem;

  // ---- Policy Decision Types ----

  enum class policy_decision
  {
    ALLOW,
    REQUIRE_APPROVAL,
    DENY
  };

  // ---- Policy Context ----

  struct policy_context
  {
    std::string tool_name;
    json tool_arguments;
    std::string working_directory;

    // Normalized paths for matching (computed internally)
    std::optional<std::string> normalized_path;
    std::optional<std::string> normalized_cwd;

    // For EXECUTABLE rule type - extracted command name from exec tool arguments
    std::optional<std::string> command_name;
    // For EXECUTABLE rule type - extracted command args from exec tool arguments
    std::optional<std::vector<std::string>> command_args;
    // For EXECUTABLE rule type - absolute path of the resolved executable
    // (PATH lookup for bare names, process-cwd-relative for paths)
    std::optional<std::string> resolved_command_path;
  };

  // ---- Policy Result ----

  struct policy_result
  {
    policy_decision decision;
    std::optional<std::string> reason;
    std::optional<std::string> rejection_message;
  };

  // ---- Rule Types ----

  enum class rule_type
  {
    TOOL,
    DIRECTORY,
    PATH_PREFIX,
    EXECUTABLE,
    UNKNOWN
  };
  enum class rule_action
  {
    ALLOW,
    REQUIRE_APPROVAL
  };

  // ---- Policy Rule ----

  struct policy_rule
  {
    rule_action action;
    rule_type type;
    std::string tool_name;
    std::string path;
    std::vector<std::string> exclusions;
    std::string path_prefix;
    bool path_negated = false;
    std::string id;
    std::optional<std::string> custom_message;

    // For EXECUTABLE rule type - command name matching
    std::string command;
    // For EXECUTABLE rule type - optional args constraint
    std::vector<std::string> args;
    // For EXECUTABLE rule type - optional working directory constraint
    std::optional<std::string> cwd;
  };

  // ---- Policy Engine ----

  class policy_engine
  {
  public:
    policy_engine();

    // Load configuration from JSON
    void load_configuration(const json& config);

    // Add a rule using DSL string
    void add_rule(const std::string& rule_dsl);

    // Add rules programmatically
    void allow_tool(const std::string& tool_name);
    void allow_directory(
      const std::string& tool_name,
      const std::string& path,
      const std::vector<std::string>& exclude = {}
    );
    void require_approval_tool(const std::string& tool_name);
    void require_approval_directory(const std::string& tool_name, const std::string& path_pattern);
    void require_approval_path_prefix(const std::string& tool_name, const std::string& prefix);

    // Executable-specific rules (for exec tool)
    void allow_executable(
      const std::string& tool_name,
      const std::string& command,
      const std::vector<std::string>& args = {},
      const std::optional<std::string>& cwd = std::nullopt
    );
    void require_approval_executable(
      const std::string& tool_name,
      const std::string& command,
      const std::vector<std::string>& args = {},
      const std::optional<std::string>& cwd = std::nullopt
    );

    // Set default policy
    void set_default_policy(policy_decision default_policy);

    // Set custom rejection message for a rule
    void set_rejection_message(const std::string& rule_id, const std::string& message);

    // Evaluate a tool call against policies
    policy_result evaluate(const policy_context& ctx) const;

    // Normalize a path for policy matching
    static std::string normalize_path(const std::string& path);

    // Get current default policy
    policy_decision get_default_policy() const
    {
      return default_policy_;
    }

    // Clear all rules
    void clear_rules();

    // Get number of rules loaded
    size_t rule_count() const
    {
      return rules_.size();
    }

  private:
    // Parse rule fields from JSON (used by load_configuration)
    void parse_rule_fields(policy_rule& rule, const json& rule_json) const;

    // Parse DSL rule body (selector + additional parts)
    void parse_dsl_rule_body(
      policy_rule& rule,
      const std::string& selector,
      const std::vector<std::string>& parts
    ) const;

    // Parse a DSL rule string
    policy_rule parse_dsl_rule(const std::string& rule_dsl) const;

    // Normalize context paths and extract exec-specific fields
    policy_context normalize_context(const policy_context& ctx) const;

    // Check if a rule matches the context
    bool rule_matches(const policy_rule& rule, const policy_context& ctx) const;

    // Check if command matches (supports glob patterns)
    bool command_matches(const std::string& rule_command, const std::string& test_command) const;

    // Check path matching
    bool path_matches(
      const std::string& rule_path,
      const std::string& test_path,
      bool negated,
      const std::vector<std::string>& exclusions
    ) const;

    // Check if path is excluded
    bool is_excluded(const std::string& path, const std::vector<std::string>& exclusions) const;

    // Generate rule ID
    std::string generate_rule_id(const policy_rule& rule) const;

    // Helper to get or create regex from cache
    const std::regex& get_cached_regex(const std::string& pattern) const;

    // Check if args match
    bool args_match(const std::vector<std::string>& rule_args, const std::vector<std::string>& test_args) const;

    // Check if cwd matches
    bool cwd_matches(const std::optional<std::string>& rule_cwd, const std::optional<std::string>& test_cwd) const;

    // Check if the resolved executable is the binary a rule names
    // (<rule.cwd>/<rule.command>)
    bool executable_path_matches(const policy_rule& rule, const std::string& resolved_path) const;

  private:
    std::vector<policy_rule> rules_;
    policy_decision default_policy_ = policy_decision::DENY;
    std::map<std::string, std::string> rejection_messages_;
    mutable std::map<std::string, std::regex> regex_cache_;
  };

} // namespace mistral::agent

#endif // MISTRAL_POLICY_ENGINE_H
