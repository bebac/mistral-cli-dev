#ifndef MISTRAL_POLICY_ENGINE_H
#define MISTRAL_POLICY_ENGINE_H

#include <nlohmann/json.hpp>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <memory>
#include <regex>

namespace mistral::agent
{
  using json = nlohmann::ordered_json;

  namespace fs = std::filesystem;

  // ---- Policy Decision Types ----

  enum class policy_decision { ALLOW, REQUIRE_APPROVAL, DENY };

  // ---- Policy Context ----

  struct policy_context
  {
    std::string tool_name;
    json tool_arguments;
    std::string working_directory;
    
    // Normalized paths for matching (computed internally)
    std::optional<std::string> normalized_path;
    std::optional<std::string> normalized_cwd;
  };

  // ---- Policy Result ----

  struct policy_result
  {
    policy_decision decision;
    std::optional<std::string> reason;
    std::optional<std::string> rejection_message;
  };

  // ---- Rule Types ----

  enum class rule_type { TOOL, DIRECTORY, PATH_PREFIX, UNKNOWN };
  enum class rule_action { ALLOW, REQUIRE_APPROVAL };

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
    void allow_directory(const std::string& tool_name,
                         const std::string& path,
                         const std::vector<std::string>& exclude = {});
    void require_approval_tool(const std::string& tool_name);
    void require_approval_directory(const std::string& tool_name,
                                   const std::string& path_pattern);
    void require_approval_path_prefix(const std::string& tool_name,
                                      const std::string& prefix);
    
    // Set default policy
    void set_default_policy(policy_decision default_policy);
    
    // Set custom rejection message for a rule
    void set_rejection_message(const std::string& rule_id,
                               const std::string& message);
    
    // Evaluate a tool call against policies
    policy_result evaluate(const policy_context& ctx) const;
    
    // Normalize a path for policy matching
    static std::string normalize_path(const std::string& path);
    
    // Get current default policy
    policy_decision get_default_policy() const { return default_policy_; }
    
    // Clear all rules
    void clear_rules();
    
    // Get number of rules loaded
    size_t rule_count() const { return rules_.size(); }

  private:
    // Parse a DSL rule string
    policy_rule parse_dsl_rule(const std::string& rule_dsl) const;
    
    // Normalize context paths
    policy_context normalize_context(const policy_context& ctx) const;
    
    // Check if a rule matches the context
    bool rule_matches(const policy_rule& rule, const policy_context& ctx) const;
    
    // Check path matching
    bool path_matches(const std::string& rule_path, const std::string& test_path,
                      bool negated, const std::vector<std::string>& exclusions) const;
    
    // Check if path is excluded
    bool is_excluded(const std::string& path, const std::vector<std::string>& exclusions) const;
    
    // Generate rule ID
    std::string generate_rule_id(const policy_rule& rule) const;
    
    // Helper to get or create regex from cache
    const std::regex& get_cached_regex(const std::string& pattern) const;

  private:
    std::vector<policy_rule> rules_;
    policy_decision default_policy_ = policy_decision::DENY;
    std::map<std::string, std::string> rejection_messages_;
    mutable std::map<std::string, std::regex> regex_cache_;
  };

} // namespace mistral::agent

#endif // MISTRAL_POLICY_ENGINE_H
