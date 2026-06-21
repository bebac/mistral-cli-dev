#include "agent/policy_engine.h"
#include "common/string_utils.h"

#include <algorithm>
#include <sstream>
#include <cctype>
#include <format>
#include <regex>
#include <ranges>

namespace mistral::agent
{

// ---- Static Helpers ----

namespace
{
  using namespace mistral::utils;
}

// ---- policy_engine Implementation ----

policy_engine::policy_engine() = default;

const std::regex& policy_engine::get_cached_regex(const std::string& pattern) const
{
  auto it = regex_cache_.find(pattern);
  if (it != regex_cache_.end()) {
    return it->second;
  }
  // Insert and return
  try {
    auto result = regex_cache_.emplace(pattern, std::regex(pattern));
    return result.first->second;
  } catch (const std::regex_error&) {
    // Return a dummy regex that never matches
    static const std::regex dummy("a^"); // Never matches
    return dummy;
  }
}

void policy_engine::load_configuration(const json& config)
{
  clear_rules();

  // Load default policy
  if (config.contains("default")) {
    std::string default_str = config["default"].get<std::string>();
    if (to_lower(default_str) == "allow") {
      default_policy_ = policy_decision::ALLOW;
    } else if (to_lower(default_str) == "require_approval") {
      default_policy_ = policy_decision::REQUIRE_APPROVAL;
    } else {
      default_policy_ = policy_decision::DENY;
    }
  }

  // Load rejection messages
  if (config.contains("rejection_messages")) {
    for (auto it = config["rejection_messages"].begin();
         it != config["rejection_messages"].end(); ++it) {
      rejection_messages_[it.key()] = it.value().get<std::string>();
    }
  }

  // Load allow rules
  if (config.contains("allow")) {
    for (const auto& rule_json : config["allow"]) {
      policy_rule rule;
      rule.action = rule_action::ALLOW;

      if (rule_json.contains("type")) {
        std::string type = rule_json["type"].get<std::string>();
        if (type == "tool") {
          rule.type = rule_type::TOOL;
          rule.tool_name = rule_json["tool"].get<std::string>();
        } else if (type == "directory") {
          rule.type = rule_type::DIRECTORY;
          rule.tool_name = rule_json.contains("tool") ?
                          rule_json["tool"].get<std::string>() : "*";
          rule.path = rule_json["path"].get<std::string>();
          rule.path_negated = starts_with(rule.path, "!");
          if (rule.path_negated) {
            rule.path = normalize_path(rule.path.substr(1));
          } else {
            rule.path = normalize_path(rule.path);
          }
          if (rule_json.contains("exclude")) {
            for (const auto& excl : rule_json["exclude"]) {
              rule.exclusions.push_back(fs::path(rule.path) / excl.get<std::string>());
            }
          }
        } else if (type == "path_prefix") {
          rule.type = rule_type::PATH_PREFIX;
          rule.tool_name = rule_json.contains("tool") ?
                          rule_json["tool"].get<std::string>() : "*";
          rule.path_prefix = normalize_path(rule_json["prefix"].get<std::string>());
        }
      }

      if (rule_json.contains("id")) {
        rule.id = rule_json["id"].get<std::string>();
      } else {
        rule.id = generate_rule_id(rule);
      }

      if (rule_json.contains("message")) {
        rule.custom_message = rule_json["message"].get<std::string>();
      }

      rules_.push_back(std::move(rule));
    }
  }

  // Load require_approval rules
  if (config.contains("require_approval")) {
    for (const auto& rule_json : config["require_approval"]) {
      policy_rule rule;
      rule.action = rule_action::REQUIRE_APPROVAL;

      if (rule_json.contains("type")) {
        std::string type = rule_json["type"].get<std::string>();
        if (type == "tool") {
          rule.type = rule_type::TOOL;
          rule.tool_name = rule_json["tool"].get<std::string>();
        } else if (type == "directory") {
          rule.type = rule_type::DIRECTORY;
          rule.tool_name = rule_json.contains("tool") ?
                          rule_json["tool"].get<std::string>() : "*";
          rule.path = rule_json["path"].get<std::string>();
          rule.path_negated = starts_with(rule.path, "!");
          if (rule.path_negated) {
            rule.path = normalize_path(rule.path.substr(1));
          } else {
            rule.path = normalize_path(rule.path);
          }
          if (rule_json.contains("exclude")) {
            for (const auto& excl : rule_json["exclude"]) {
              rule.exclusions.push_back(fs::path(rule.path) / excl.get<std::string>());
            }
          }
        } else if (type == "path_prefix") {
          rule.type = rule_type::PATH_PREFIX;
          rule.tool_name = rule_json.contains("tool") ?
                          rule_json["tool"].get<std::string>() : "*";
          rule.path_prefix = normalize_path(rule_json["prefix"].get<std::string>());
        }
      }

      if (rule_json.contains("id")) {
        rule.id = rule_json["id"].get<std::string>();
      } else {
        rule.id = generate_rule_id(rule);
      }

      if (rule_json.contains("message")) {
        rule.custom_message = rule_json["message"].get<std::string>();
      }

      rules_.push_back(std::move(rule));
    }
  }
}

void policy_engine::add_rule(const std::string& rule_dsl)
{
  policy_rule rule = parse_dsl_rule(rule_dsl);
  if (rule.type != rule_type::UNKNOWN) {
    if (rule.id.empty()) {
      rule.id = generate_rule_id(rule);
    }
    rules_.push_back(std::move(rule));
  }
}

void policy_engine::allow_tool(const std::string& tool_name)
{
  policy_rule rule;
  rule.action = rule_action::ALLOW;
  rule.type = rule_type::TOOL;
  rule.tool_name = tool_name;
  rule.id = generate_rule_id(rule);
  rules_.push_back(std::move(rule));
}

void policy_engine::allow_directory(const std::string& tool_name,
                                   const std::string& path,
                                   const std::vector<std::string>& exclude)
{
  policy_rule rule;
  rule.action = rule_action::ALLOW;
  rule.type = rule_type::DIRECTORY;
  rule.tool_name = tool_name;
  rule.path = normalize_path(path);  // Normalize the path when storing
  rule.path_negated = false;

  // Prepend rule.path to each entry in exclude
  std::vector<std::string> transformed_exclusions;
  transformed_exclusions.reserve(exclude.size());
  for (const auto& entry : exclude) {
    transformed_exclusions.push_back(fs::path(rule.path) / entry);
  }
  rule.exclusions = std::move(transformed_exclusions);

  //rule.exclusions = exclude;
  rule.id = generate_rule_id(rule);
  rules_.push_back(std::move(rule));
}

void policy_engine::require_approval_tool(const std::string& tool_name)
{
  policy_rule rule;
  rule.action = rule_action::REQUIRE_APPROVAL;
  rule.type = rule_type::TOOL;
  rule.tool_name = tool_name;
  rule.id = generate_rule_id(rule);
  rules_.push_back(std::move(rule));
}

void policy_engine::require_approval_directory(const std::string& tool_name,
                                             const std::string& path_pattern)
{
  policy_rule rule;
  rule.action = rule_action::REQUIRE_APPROVAL;
  rule.type = rule_type::DIRECTORY;
  rule.tool_name = tool_name;
  rule.path_negated = starts_with(path_pattern, "!");
  if (rule.path_negated) {
    rule.path = normalize_path(path_pattern.substr(1));
  } else {
    rule.path = normalize_path(path_pattern);
  }
  rule.id = generate_rule_id(rule);
  rules_.push_back(std::move(rule));
}

void policy_engine::require_approval_path_prefix(const std::string& tool_name,
                                                const std::string& prefix)
{
  policy_rule rule;
  rule.action = rule_action::REQUIRE_APPROVAL;
  rule.type = rule_type::PATH_PREFIX;
  rule.tool_name = tool_name;
  rule.path_prefix = normalize_path(prefix);  // Normalize the prefix when storing
  rule.id = generate_rule_id(rule);
  rules_.push_back(std::move(rule));
}

void policy_engine::set_default_policy(policy_decision default_policy)
{
  default_policy_ = default_policy;
}

void policy_engine::set_rejection_message(const std::string& rule_id,
                                         const std::string& message)
{
  rejection_messages_[rule_id] = message;
}

policy_result policy_engine::evaluate(const policy_context& ctx) const
{
  policy_context normalized_ctx = normalize_context(ctx);

  // Priority order: DIRECTORY > PATH_PREFIX > TOOL
  // Within each type, check in reverse order (most recently added first)

  // Check DIRECTORY rules first (most specific)
  for (auto it = rules_.rbegin(); it != rules_.rend(); ++it) {
    const auto& rule = *it;
    if (rule.type == rule_type::DIRECTORY && rule_matches(rule, normalized_ctx)) {
      std::optional<std::string> msg = rule.custom_message;
      if (!msg && !rule.id.empty()) {
        auto it2 = rejection_messages_.find(rule.id);
        if (it2 != rejection_messages_.end()) {
          msg = it2->second;
        }
      }
      if (rule.action == rule_action::ALLOW) {
        return policy_result{
          policy_decision::ALLOW,
          std::format("Allowed by rule: {}", rule.id),
          std::nullopt
        };
      } else {
        return policy_result{
          policy_decision::REQUIRE_APPROVAL,
          std::format("Requires approval by rule: {}", rule.id),
          msg
        };
      }
    }
  }

  // Check PATH_PREFIX rules
  for (auto it = rules_.rbegin(); it != rules_.rend(); ++it) {
    const auto& rule = *it;
    if (rule.type == rule_type::PATH_PREFIX && rule_matches(rule, normalized_ctx)) {
      std::optional<std::string> msg = rule.custom_message;
      if (!msg && !rule.id.empty()) {
        auto it2 = rejection_messages_.find(rule.id);
        if (it2 != rejection_messages_.end()) {
          msg = it2->second;
        }
      }
      if (rule.action == rule_action::ALLOW) {
        return policy_result{
          policy_decision::ALLOW,
          std::format("Allowed by rule: {}", rule.id),
          std::nullopt
        };
      } else {
        return policy_result{
          policy_decision::REQUIRE_APPROVAL,
          std::format("Requires approval by rule: {}", rule.id),
          msg
        };
      }
    }
  }

  // Check TOOL rules
  for (auto it = rules_.rbegin(); it != rules_.rend(); ++it) {
    const auto& rule = *it;
    if (rule.type == rule_type::TOOL && rule_matches(rule, normalized_ctx)) {
      std::optional<std::string> msg = rule.custom_message;
      if (!msg && !rule.id.empty()) {
        auto it2 = rejection_messages_.find(rule.id);
        if (it2 != rejection_messages_.end()) {
          msg = it2->second;
        }
      }
      if (rule.action == rule_action::ALLOW) {
        return policy_result{
          policy_decision::ALLOW,
          std::format("Allowed by rule: {}", rule.id),
          std::nullopt
        };
      } else {
        return policy_result{
          policy_decision::REQUIRE_APPROVAL,
          std::format("Requires approval by rule: {}", rule.id),
          msg
        };
      }
    }
  }

  // Step 3: Apply default policy
  if (default_policy_ == policy_decision::ALLOW) {
    return policy_result{
      policy_decision::ALLOW,
      "Default policy: allow",
      std::nullopt
    };
  } else if (default_policy_ == policy_decision::REQUIRE_APPROVAL) {
    return policy_result{
      policy_decision::REQUIRE_APPROVAL,
      "Default policy: require approval",
      std::nullopt
    };
  } else {
    return policy_result{
      policy_decision::DENY,
      "Default policy: deny",
      std::nullopt
    };
  }
}

std::string policy_engine::normalize_path(const std::string& path)
{
  try {
    fs::path p = path;

    // If relative, make absolute first
    if (p.is_relative()) {
      p = fs::absolute(p);
    }

    // Use weakly_canonical to normalize (resolves symlinks where possible)
    fs::path normalized = fs::weakly_canonical(p);
    return normalized.string();
  } catch (const fs::filesystem_error& e) {
    // Fallback to absolute path if normalization fails
    try {
      return fs::absolute(path).string();
    } catch (...) {
      return path;
    }
  }
}

policy_context policy_engine::normalize_context(const policy_context& ctx) const
{
  policy_context normalized = ctx;

  // Normalize working directory
  if (!ctx.working_directory.empty()) {
    normalized.normalized_cwd = normalize_path(ctx.working_directory);
  }

  // Normalize path argument if present
  if (ctx.tool_arguments.contains("path")) {
    std::string path = ctx.tool_arguments["path"].get<std::string>();
    normalized.normalized_path = normalize_path(path);
  }

  return normalized;
}

policy_rule policy_engine::parse_dsl_rule(const std::string& rule_dsl) const
{
  policy_rule rule;
  rule.id = rule_dsl; // Default ID is the DSL string itself

  // Parse format: [action]:[selector]=[value]
  // or: [action]:[type]:[selector]=[value]

  std::vector<std::string> parts = split_comma(rule_dsl);
  if (parts.empty()) return rule;

  std::string first_part = parts[0];

  // Check for action
  if (starts_with(first_part, "allow:")) {
    rule.action = rule_action::ALLOW;
    std::string rest = first_part.substr(6);

    if (starts_with(rest, "tool=")) {
      rule.type = rule_type::TOOL;
      rule.tool_name = rest.substr(5);
    } else if (starts_with(rest, "write:dir=") || starts_with(rest, "read:dir=")) {
      size_t colon_pos = rest.find(':');
      rule.tool_name = rest.substr(0, colon_pos);
      std::string dir_part = rest.substr(colon_pos + 1); // "dir=..."

      if (starts_with(dir_part, "dir=")) {
        rule.type = rule_type::DIRECTORY;
        std::string path = dir_part.substr(4);
        rule.path_negated = starts_with(path, "!");
        if (rule.path_negated) {
          rule.path = normalize_path(path.substr(1));
        } else {
          rule.path = normalize_path(path);
        }
      }
    } else if (starts_with(rest, "write:path_prefix=") ||
               starts_with(rest, "read:path_prefix=")) {
      size_t colon_pos = rest.find(':');
      rule.tool_name = rest.substr(0, colon_pos);
      std::string prefix_part = rest.substr(colon_pos + 1);

      if (starts_with(prefix_part, "path_prefix=")) {
        rule.type = rule_type::PATH_PREFIX;
        rule.path_prefix = normalize_path(prefix_part.substr(12));
      }
    }

    // Parse additional parts (exclusions, etc.)
    for (size_t i = 1; i < parts.size(); ++i) {
      if (starts_with(parts[i], "exclude=")) {
        std::string excl_str = parts[i].substr(8);
        for (const auto& excl : split_comma(excl_str)) {
          rule.exclusions.push_back(fs::path(rule.path) / excl);
        }
      }
    }

  } else if (starts_with(first_part, "require:") ||
             starts_with(first_part, "require_approval:")) {
    rule.action = rule_action::REQUIRE_APPROVAL;
    size_t action_end = first_part.find(':');
    std::string rest = first_part.substr(action_end + 1);

    if (starts_with(rest, "tool=")) {
      rule.type = rule_type::TOOL;
      rule.tool_name = rest.substr(5);
    } else if (starts_with(rest, "write:dir=") || starts_with(rest, "read:dir=")) {
      size_t colon_pos = rest.find(':');
      rule.tool_name = rest.substr(0, colon_pos);
      std::string dir_part = rest.substr(colon_pos + 1);

      if (starts_with(dir_part, "dir=")) {
        rule.type = rule_type::DIRECTORY;
        std::string path = dir_part.substr(4);
        rule.path_negated = starts_with(path, "!");
        if (rule.path_negated) {
          rule.path = normalize_path(path.substr(1));
        } else {
          rule.path = normalize_path(path);
        }
      }
    } else if (starts_with(rest, "write:path_prefix=") ||
               starts_with(rest, "read:path_prefix=")) {
      size_t colon_pos = rest.find(':');
      rule.tool_name = rest.substr(0, colon_pos);
      std::string prefix_part = rest.substr(colon_pos + 1);

      if (starts_with(prefix_part, "path_prefix=")) {
        rule.type = rule_type::PATH_PREFIX;
        rule.path_prefix = normalize_path(prefix_part.substr(12));
      }
    }
  } else if (starts_with(first_part, "default=")) {
    // This is a default policy setting, not a rule
    rule.type = rule_type::UNKNOWN;
  }

  return rule;
}

bool policy_engine::rule_matches(const policy_rule& rule, const policy_context& ctx) const
{
  // Check tool name match
  if (rule.tool_name != "*" && rule.tool_name != ctx.tool_name) {
    return false;
  }

  switch (rule.type) {
    case rule_type::TOOL:
      // Tool name already matched above
      return true;

    case rule_type::DIRECTORY: {
      // Need a path argument
      if (!ctx.normalized_path) return false;

      return path_matches(rule.path, *ctx.normalized_path,
                          rule.path_negated, rule.exclusions);
    }

    case rule_type::PATH_PREFIX: {
      // Need a path argument
      if (!ctx.normalized_path) return false;

      const std::string& path = *ctx.normalized_path;
      const std::string& prefix = rule.path_prefix;

      // Check if path starts with prefix
      if (starts_with(path, prefix)) {
        // Make sure it's a proper prefix match (not just partial match)
        if (path.size() == prefix.size() ||
            path[prefix.size()] == '/' ||
            prefix.back() == '/') {
          return true;
        }
      }
      return false;
    }

    case rule_type::UNKNOWN:
    default:
      return false;
  }
}

bool policy_engine::path_matches(const std::string& rule_path,
                               const std::string& test_path,
                               bool negated,
                               const std::vector<std::string>& exclusions) const
{
  // Both paths should already be normalized at this point
  std::string norm_rule = rule_path;
  std::string norm_test = test_path;

  // Ensure both end with separator for directory matching
  if (!norm_rule.empty() && norm_rule.back() != '/') {
    norm_rule += '/';
  }

  // Check if test path starts with rule path
  bool matches = starts_with(norm_test, norm_rule) ||
                 starts_with(norm_test, norm_rule.substr(0, norm_rule.size() - 1));

  if (negated) {
    matches = !matches;
  }

  // Check exclusions
  if (matches && !exclusions.empty()) {
    if (is_excluded(norm_test, exclusions)) {
      return false;
    }
  }

  return matches;
}

bool policy_engine::is_excluded(const std::string& path,
                              const std::vector<std::string>& exclusions) const
{
  for (const auto& excl : exclusions) {
    std::string norm_excl = normalize_path(excl);
    if (!norm_excl.empty() && norm_excl.back() != '/') {
      norm_excl += '/';
    }

    if (starts_with(path, norm_excl) ||
        starts_with(path, norm_excl.substr(0, norm_excl.size() - 1))) {
      return true;
    }

    // Also check for glob pattern matching
    if (excl.find('*') != std::string::npos ||
        excl.find('?') != std::string::npos) {
      std::string pattern = glob_to_regex(excl);
      try {
        const std::regex& re = get_cached_regex(pattern);
        if (std::regex_match(path, re)) {
          return true;
        }
      } catch (...) {
        // If regex compilation fails, skip
      }
    }
  }
  return false;
}

std::string policy_engine::generate_rule_id(const policy_rule& rule) const
{
  std::ostringstream oss;
  oss << (rule.action == rule_action::ALLOW ? "allow" : "require")
      << ":" << static_cast<int>(rule.type)
      << ":" << rule.tool_name;

  switch (rule.type) {
    case rule_type::DIRECTORY:
      oss << ":dir=" << rule.path;
      if (rule.path_negated) oss << "!";
      if (!rule.exclusions.empty()) {
        oss << ",exclude=";
        for (size_t i = 0; i < rule.exclusions.size(); ++i) {
          if (i > 0) oss << ",";
          oss << rule.exclusions[i];
        }
      }
      break;
    case rule_type::PATH_PREFIX:
      oss << ":prefix=" << rule.path_prefix;
      break;
    case rule_type::TOOL:
    case rule_type::UNKNOWN:
      break;
  }

  return oss.str();
}

void policy_engine::clear_rules()
{
  rules_.clear();
  regex_cache_.clear();
}

} // namespace mistral::agent
