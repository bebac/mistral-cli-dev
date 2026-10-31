#include "agent/policy_engine.h"
#include "common/executable_finder.h"
#include "common/string_utils.h"
#include "mcp/mcp_manager.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <ranges>
#include <regex>
#include <sstream>

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
    if (it != regex_cache_.end())
    {
      return it->second;
    }
    // Insert and return
    try
    {
      auto result = regex_cache_.emplace(pattern, std::regex(pattern));
      return result.first->second;
    }
    catch (const std::regex_error&)
    {
      // Return a dummy regex that never matches
      static const std::regex dummy("a^"); // Never matches
      return dummy;
    }
  }

  void policy_engine::load_configuration(const json& config)
  {
    clear_rules();

    // Load default policy
    if (config.contains("default"))
    {
      std::string default_str = config["default"].get<std::string>();
      if (to_lower(default_str) == "allow")
      {
        default_policy_ = policy_decision::ALLOW;
      }
      else if (to_lower(default_str) == "require_approval")
      {
        default_policy_ = policy_decision::REQUIRE_APPROVAL;
      }
      else
      {
        default_policy_ = policy_decision::DENY;
      }
    }

    // Load rejection messages
    if (config.contains("rejection_messages"))
    {
      for (auto it = config["rejection_messages"].begin(); it != config["rejection_messages"].end(); ++it)
      {
        rejection_messages_[it.key()] = it.value().get<std::string>();
      }
    }

    // Load allow rules
    if (config.contains("allow"))
    {
      for (const auto& rule_json : config["allow"])
      {
        policy_rule rule;
        rule.action = rule_action::ALLOW;
        parse_rule_fields(rule, rule_json);
        rules_.push_back(std::move(rule));
      }
    }

    // Load require_approval rules
    if (config.contains("require_approval"))
    {
      for (const auto& rule_json : config["require_approval"])
      {
        policy_rule rule;
        rule.action = rule_action::REQUIRE_APPROVAL;
        parse_rule_fields(rule, rule_json);
        rules_.push_back(std::move(rule));
      }
    }
  }

  void policy_engine::parse_rule_fields(policy_rule& rule, const json& rule_json) const
  {
    if (rule_json.contains("type"))
    {
      std::string type = rule_json["type"].get<std::string>();
      if (type == "tool")
      {
        rule.type = rule_type::TOOL;
        rule.tool_name = rule_json["tool"].get<std::string>();
      }
      else if (type == "directory")
      {
        rule.type = rule_type::DIRECTORY;
        rule.tool_name = rule_json.contains("tool") ? rule_json["tool"].get<std::string>() : "*";
        rule.path = rule_json["path"].get<std::string>();
        rule.path_negated = rule.path.starts_with("!");
        if (rule.path_negated)
        {
          rule.path = normalize_path(rule.path.substr(1));
        }
        else
        {
          rule.path = normalize_path(rule.path);
        }
        if (rule_json.contains("exclude"))
        {
          for (const auto& excl : rule_json["exclude"])
          {
            rule.exclusions.push_back(fs::path(rule.path) / excl.get<std::string>());
          }
        }
      }
      else if (type == "path_prefix")
      {
        rule.type = rule_type::PATH_PREFIX;
        rule.tool_name = rule_json.contains("tool") ? rule_json["tool"].get<std::string>() : "*";
        rule.path_prefix = normalize_path(rule_json["prefix"].get<std::string>());
      }
      else if (type == "executable")
      {
        rule.type = rule_type::EXECUTABLE;
        rule.tool_name = rule_json.contains("tool") ? rule_json["tool"].get<std::string>() : "exec";
        rule.command = rule_json["command"].get<std::string>();
        if (rule_json.contains("args"))
        {
          for (const auto& arg : rule_json["args"])
          {
            rule.args.push_back(arg.get<std::string>());
          }
        }
        if (rule_json.contains("cwd"))
        {
          rule.cwd = normalize_path(rule_json["cwd"].get<std::string>());
        }
      }
    }

    if (rule_json.contains("id"))
    {
      rule.id = rule_json["id"].get<std::string>();
    }
    else
    {
      rule.id = generate_rule_id(rule);
    }

    if (rule_json.contains("message"))
    {
      rule.custom_message = rule_json["message"].get<std::string>();
    }
  }

  void policy_engine::add_rule(const std::string& rule_dsl)
  {
    policy_rule rule = parse_dsl_rule(rule_dsl);
    if (rule.type != rule_type::UNKNOWN)
    {
      if (rule.id.empty())
      {
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

  void policy_engine::allow_directory(
    const std::string& tool_name,
    const std::string& path,
    const std::vector<std::string>& exclude
  )
  {
    policy_rule rule;
    rule.action = rule_action::ALLOW;
    rule.type = rule_type::DIRECTORY;
    rule.tool_name = tool_name;
    rule.path = normalize_path(path); // Normalize the path when storing
    rule.path_negated = false;

    // Prepend rule.path to each entry in exclude
    std::vector<std::string> transformed_exclusions;
    transformed_exclusions.reserve(exclude.size());
    for (const auto& entry : exclude)
    {
      transformed_exclusions.push_back(fs::path(rule.path) / entry);
    }
    rule.exclusions = std::move(transformed_exclusions);

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

  void policy_engine::require_approval_directory(const std::string& tool_name, const std::string& path_pattern)
  {
    policy_rule rule;
    rule.action = rule_action::REQUIRE_APPROVAL;
    rule.type = rule_type::DIRECTORY;
    rule.tool_name = tool_name;
    rule.path_negated = path_pattern.starts_with("!");
    if (rule.path_negated)
    {
      rule.path = normalize_path(path_pattern.substr(1));
    }
    else
    {
      rule.path = normalize_path(path_pattern);
    }
    rule.id = generate_rule_id(rule);
    rules_.push_back(std::move(rule));
  }

  void policy_engine::require_approval_path_prefix(const std::string& tool_name, const std::string& prefix)
  {
    policy_rule rule;
    rule.action = rule_action::REQUIRE_APPROVAL;
    rule.type = rule_type::PATH_PREFIX;
    rule.tool_name = tool_name;
    rule.path_prefix = normalize_path(prefix); // Normalize the prefix when storing
    rule.id = generate_rule_id(rule);
    rules_.push_back(std::move(rule));
  }

  void policy_engine::allow_executable(
    const std::string& tool_name,
    const std::string& command,
    const std::vector<std::string>& args,
    const std::optional<std::string>& cwd
  )
  {
    policy_rule rule;
    rule.action = rule_action::ALLOW;
    rule.type = rule_type::EXECUTABLE;
    rule.tool_name = tool_name;
    rule.command = command;
    rule.args = args;
    if (cwd)
    {
      rule.cwd = normalize_path(*cwd);
    }
    rule.id = generate_rule_id(rule);
    rules_.push_back(std::move(rule));
  }

  void policy_engine::require_approval_executable(
    const std::string& tool_name,
    const std::string& command,
    const std::vector<std::string>& args,
    const std::optional<std::string>& cwd
  )
  {
    policy_rule rule;
    rule.action = rule_action::REQUIRE_APPROVAL;
    rule.type = rule_type::EXECUTABLE;
    rule.tool_name = tool_name;
    rule.command = command;
    rule.args = args;
    if (cwd)
    {
      rule.cwd = normalize_path(*cwd);
    }
    rule.id = generate_rule_id(rule);
    rules_.push_back(std::move(rule));
  }

  void policy_engine::set_default_policy(policy_decision default_policy)
  {
    default_policy_ = default_policy;
  }

  void policy_engine::set_rejection_message(const std::string& rule_id, const std::string& message)
  {
    rejection_messages_[rule_id] = message;
  }

  policy_result policy_engine::evaluate(const policy_context& ctx) const
  {
    policy_context normalized_ctx = normalize_context(ctx);

    // Priority order: DIRECTORY > PATH_PREFIX > EXECUTABLE > TOOL
    // Within each type, check in reverse order (most recently added first)

    // Check DIRECTORY rules first (most specific)
    for (auto it = rules_.rbegin(); it != rules_.rend(); ++it)
    {
      const auto& rule = *it;
      if (rule.type == rule_type::DIRECTORY && rule_matches(rule, normalized_ctx))
      {
        std::optional<std::string> msg = rule.custom_message;
        if (!msg && !rule.id.empty())
        {
          auto it2 = rejection_messages_.find(rule.id);
          if (it2 != rejection_messages_.end())
          {
            msg = it2->second;
          }
        }
        if (rule.action == rule_action::ALLOW)
        {
          return policy_result{policy_decision::ALLOW, std::format("Allowed by rule: {}", rule.id), std::nullopt};
        }
        else
        {
          return policy_result{
            policy_decision::REQUIRE_APPROVAL, std::format("Requires approval by rule: {}", rule.id), msg
          };
        }
      }
    }

    // Check PATH_PREFIX rules
    for (auto it = rules_.rbegin(); it != rules_.rend(); ++it)
    {
      const auto& rule = *it;
      if (rule.type == rule_type::PATH_PREFIX && rule_matches(rule, normalized_ctx))
      {
        std::optional<std::string> msg = rule.custom_message;
        if (!msg && !rule.id.empty())
        {
          auto it2 = rejection_messages_.find(rule.id);
          if (it2 != rejection_messages_.end())
          {
            msg = it2->second;
          }
        }
        if (rule.action == rule_action::ALLOW)
        {
          return policy_result{policy_decision::ALLOW, std::format("Allowed by rule: {}", rule.id), std::nullopt};
        }
        else
        {
          return policy_result{
            policy_decision::REQUIRE_APPROVAL, std::format("Requires approval by rule: {}", rule.id), msg
          };
        }
      }
    }

    // Check EXECUTABLE rules
    for (auto it = rules_.rbegin(); it != rules_.rend(); ++it)
    {
      const auto& rule = *it;
      if (rule.type == rule_type::EXECUTABLE && rule_matches(rule, normalized_ctx))
      {
        std::optional<std::string> msg = rule.custom_message;
        if (!msg && !rule.id.empty())
        {
          auto it2 = rejection_messages_.find(rule.id);
          if (it2 != rejection_messages_.end())
          {
            msg = it2->second;
          }
        }
        if (rule.action == rule_action::ALLOW)
        {
          return policy_result{policy_decision::ALLOW, std::format("Allowed by rule: {}", rule.id), std::nullopt};
        }
        else
        {
          return policy_result{
            policy_decision::REQUIRE_APPROVAL, std::format("Requires approval by rule: {}", rule.id), msg
          };
        }
      }
    }

    // Check TOOL rules
    for (auto it = rules_.rbegin(); it != rules_.rend(); ++it)
    {
      const auto& rule = *it;
      if (rule.type == rule_type::TOOL && rule_matches(rule, normalized_ctx))
      {
        std::optional<std::string> msg = rule.custom_message;
        if (!msg && !rule.id.empty())
        {
          auto it2 = rejection_messages_.find(rule.id);
          if (it2 != rejection_messages_.end())
          {
            msg = it2->second;
          }
        }
        if (rule.action == rule_action::ALLOW)
        {
          return policy_result{policy_decision::ALLOW, std::format("Allowed by rule: {}", rule.id), std::nullopt};
        }
        else
        {
          return policy_result{
            policy_decision::REQUIRE_APPROVAL, std::format("Requires approval by rule: {}", rule.id), msg
          };
        }
      }
    }

    // Step 3: Apply default policy
    if (default_policy_ == policy_decision::ALLOW)
    {
      return policy_result{policy_decision::ALLOW, "Default policy: allow", std::nullopt};
    }
    else if (default_policy_ == policy_decision::REQUIRE_APPROVAL)
    {
      return policy_result{policy_decision::REQUIRE_APPROVAL, "Default policy: require approval", std::nullopt};
    }
    else
    {
      return policy_result{policy_decision::DENY, "Default policy: deny", std::nullopt};
    }
  }

  std::string policy_engine::normalize_path(const std::string& path)
  {
    try
    {
      fs::path p = path;

      // If relative, make absolute first
      if (p.is_relative())
      {
        p = fs::absolute(p);
      }

      // Use weakly_canonical to normalize (resolves symlinks where possible)
      fs::path normalized = fs::weakly_canonical(p);
      return normalized.string();
    }
    catch (const fs::filesystem_error& e)
    {
      // Fallback to absolute path if normalization fails
      try
      {
        return fs::absolute(path).string();
      }
      catch (...)
      {
        return path;
      }
    }
  }

  policy_context policy_engine::normalize_context(const policy_context& ctx) const
  {
    policy_context normalized = ctx;

    // Normalize working directory
    if (!ctx.working_directory.empty())
    {
      normalized.normalized_cwd = normalize_path(ctx.working_directory);
    }

    // Normalize path argument if present
    if (ctx.tool_arguments.contains("path"))
    {
      std::string path = ctx.tool_arguments["path"].get<std::string>();
      normalized.normalized_path = normalize_path(path);
    }

    // For exec tool, extract command name and args
    if (ctx.tool_name == "exec" && ctx.tool_arguments.contains("command"))
    {
      normalized.command_name = ctx.tool_arguments["command"].get<std::string>();
      if (ctx.tool_arguments.contains("args") && ctx.tool_arguments["args"].is_array())
      {
        std::vector<std::string> args;
        for (const auto& arg : ctx.tool_arguments["args"])
        {
          args.push_back(arg.get<std::string>());
        }
        normalized.command_args = args;
      }
      else
      {
        normalized.command_args = {};
      }

      // Extract cwd from exec tool arguments for policy matching.
      // If not specified in tool arguments and not already set from
      // working_directory, default to current working directory — the
      // exec tool runs in the process's cwd when cwd is empty.
      std::string raw_cwd;
      if (ctx.tool_arguments.contains("cwd") && ctx.tool_arguments["cwd"].is_string())
      {
        raw_cwd = ctx.tool_arguments["cwd"].get<std::string>();
      }

      if (!normalized.normalized_cwd)
      {
        if (!raw_cwd.empty())
        {
          normalized.normalized_cwd = normalize_path(raw_cwd);
        }
        else
        {
          normalized.normalized_cwd = normalize_path(".");
        }
      }

      // Resolve the command to an absolute path, so a rule that pins a
      // cwd can match every spelling of the same binary: "tests" with
      // cwd="build", "./build/tests" and "/abs/build/tests" all resolve
      // to the same file.
      std::string resolved = resolve_executable(*normalized.command_name, raw_cwd);
      if (!resolved.empty())
      {
        normalized.resolved_command_path = normalize_path(resolved);
      }
    }

    return normalized;
  }

  bool policy_engine::command_matches(const std::string& rule_command, const std::string& test_command) const
  {
    // Exact match
    if (rule_command == test_command)
    {
      return true;
    }

    // Glob pattern matching
    if (rule_command.find('*') != std::string::npos || rule_command.find('?') != std::string::npos)
    {
      std::string pattern = glob_to_regex(rule_command);
      try
      {
        const std::regex& re = get_cached_regex(pattern);
        return std::regex_match(test_command, re);
      }
      catch (...)
      {
        // If regex compilation fails, fall back to exact match
        return rule_command == test_command;
      }
    }

    return false;
  }

  bool policy_engine::args_match(
    const std::vector<std::string>& rule_args,
    const std::vector<std::string>& test_args
  ) const
  {
    // If rule has args, test_args must contain at least one matching arg
    // This is "list match" - any of the test args must be in the rule args
    for (const auto& test_arg : test_args)
    {
      for (const auto& rule_arg : rule_args)
      {
        if (test_arg == rule_arg)
        {
          return true;
        }
        // Support glob patterns in rule args
        if (rule_arg.find('*') != std::string::npos || rule_arg.find('?') != std::string::npos)
        {
          std::string pattern = glob_to_regex(rule_arg);
          try
          {
            const std::regex& re = get_cached_regex(pattern);
            if (std::regex_match(test_arg, re))
            {
              return true;
            }
          }
          catch (...)
          {
            // If regex compilation fails, fall back to exact match
            if (test_arg == rule_arg)
            {
              return true;
            }
          }
        }
      }
    }
    return false;
  }

  bool policy_engine::cwd_matches(
    const std::optional<std::string>& rule_cwd,
    const std::optional<std::string>& test_cwd
  ) const
  {
    if (!rule_cwd || !test_cwd)
    {
      return false;
    }

    // Exact match
    if (*rule_cwd == *test_cwd)
    {
      return true;
    }

    // Check if test_cwd starts with rule_cwd (for directory matching)
    // rule_cwd = "build", test_cwd = "build/" or "build/subdir"
    std::string norm_rule = *rule_cwd;
    std::string norm_test = *test_cwd;

    if (!norm_rule.empty() && norm_rule.back() != '/')
    {
      norm_rule += '/';
    }
    if (!norm_test.empty() && norm_test.back() != '/')
    {
      norm_test += '/';
    }

    // Exact match with trailing slash
    if (norm_rule == norm_test)
    {
      return true;
    }

    // Prefix match - test_cwd starts with rule_cwd
    if (norm_test.find(norm_rule) == 0)
    {
      return true;
    }

    return false;
  }

  bool policy_engine::executable_path_matches(const policy_rule& rule, const std::string& resolved_path) const
  {
    if (!rule.cwd)
    {
      return false;
    }

    // The rule names <cwd>/<command>; both sides are normalized paths.
    std::string expected = normalize_path((fs::path(*rule.cwd) / rule.command).string());
    if (expected == resolved_path)
    {
      return true;
    }

    // Glob patterns in the command apply to the file name inside the
    // rule's directory.
    if (rule.command.find('*') != std::string::npos || rule.command.find('?') != std::string::npos)
    {
      fs::path resolved(resolved_path);
      if (normalize_path(resolved.parent_path().string()) != normalize_path(*rule.cwd))
      {
        return false;
      }
      return command_matches(rule.command, resolved.filename().string());
    }

    return false;
  }

  policy_rule policy_engine::parse_dsl_rule(const std::string& rule_dsl) const
  {
    policy_rule rule;
    rule.id = rule_dsl; // Default ID is the DSL string itself

    std::vector<std::string> parts = split_comma(rule_dsl);
    if (parts.empty())
      return rule;

    std::string first_part = parts[0];

    if (first_part.starts_with("allow:"))
    {
      rule.action = rule_action::ALLOW;
      parse_dsl_rule_body(rule, first_part.substr(6), parts);
    }
    else if (first_part.starts_with("require:") || first_part.starts_with("require_approval:"))
    {
      rule.action = rule_action::REQUIRE_APPROVAL;
      size_t action_end = first_part.find(':'); // NOLINT
      parse_dsl_rule_body(rule, first_part.substr(action_end + 1), parts);
    }
    else if (first_part.starts_with("default="))
    {
      // This is a default policy setting, not a rule
      rule.type = rule_type::UNKNOWN;
    }

    return rule;
  }

  void policy_engine::parse_dsl_rule_body(
    policy_rule& rule,
    const std::string& selector,
    const std::vector<std::string>& parts
  ) const
  {
    if (selector.starts_with("tool="))
    {
      rule.type = rule_type::TOOL;
      rule.tool_name = selector.substr(5);
    }
    else if (selector.starts_with("write:dir=") || selector.starts_with("read:dir="))
    {
      size_t colon_pos = selector.find(':'); // NOLINT
      rule.tool_name = selector.substr(0, colon_pos);
      std::string dir_part = selector.substr(colon_pos + 1); // "dir=..."

      if (dir_part.starts_with("dir="))
      {
        rule.type = rule_type::DIRECTORY;
        std::string path = dir_part.substr(4);
        rule.path_negated = path.starts_with("!");
        if (rule.path_negated)
        {
          rule.path = normalize_path(path.substr(1));
        }
        else
        {
          rule.path = normalize_path(path);
        }
      }
    }
    else if (selector.starts_with("write:path_prefix=") || selector.starts_with("read:path_prefix="))
    {
      size_t colon_pos = selector.find(':'); // NOLINT
      rule.tool_name = selector.substr(0, colon_pos);
      std::string prefix_part = selector.substr(colon_pos + 1);

      if (prefix_part.starts_with("path_prefix="))
      {
        rule.type = rule_type::PATH_PREFIX;
        rule.path_prefix = normalize_path(prefix_part.substr(12));
      }
    }
    else if (selector.starts_with("exec:command="))
    {
      rule.type = rule_type::EXECUTABLE;
      rule.tool_name = "exec";
      rule.command = selector.substr(13);
    }

    // Parse additional parts
    for (size_t i = 1; i < parts.size(); ++i)
    {
      if (parts[i].starts_with("exclude="))
      {
        std::string excl_str = parts[i].substr(8);
        for (const auto& excl : split_comma(excl_str))
        {
          rule.exclusions.push_back(fs::path(rule.path) / excl);
        }
      }
      else if (rule.type == rule_type::EXECUTABLE)
      {
        if (parts[i].starts_with("args="))
        {
          std::string args_str = parts[i].substr(5);
          rule.args = split_comma(args_str);
        }
        else if (parts[i].starts_with("cwd="))
        {
          rule.cwd = normalize_path(parts[i].substr(4));
        }
      }
    }
  }

  bool policy_engine::rule_matches(const policy_rule& rule, const policy_context& ctx) const
  {
    // Check tool name match
    if (rule.tool_name != "*" && rule.tool_name != ctx.tool_name)
    {
      // "mcp:<server>" TOOL rules match any tool registered by that
      // MCP server ("mcp:*" matches all connected servers). Connecting
      // a server is the deliberate trust decision; the rule then
      // covers all of its tools.
      if (rule.type == rule_type::TOOL && rule.tool_name.starts_with("mcp:"))
      {
        const auto& registered = mcp::mcp_manager::instance().tool_names(rule.tool_name.substr(4));
        if (std::find(registered.begin(), registered.end(), ctx.tool_name) == registered.end())
        {
          return false;
        }
      }
      else
      {
        return false;
      }
    }

    switch (rule.type)
    {
    case rule_type::TOOL:
      // Tool name already matched above
      return true;

    case rule_type::DIRECTORY:
    {
      // Need a path argument
      if (!ctx.normalized_path)
        return false;

      return path_matches(rule.path, *ctx.normalized_path, rule.path_negated, rule.exclusions);
    }

    case rule_type::PATH_PREFIX:
    {
      // Need a path argument
      if (!ctx.normalized_path)
        return false;

      const std::string& path = *ctx.normalized_path;
      const std::string& prefix = rule.path_prefix;

      // Check if path starts with prefix
      if (path.starts_with(prefix))
      {
        // Make sure it's a proper prefix match (not just partial match)
        if (path.size() == prefix.size() || path[prefix.size()] == '/' || prefix.back() == '/')
        {
          return true;
        }
      }
      return false;
    }

    case rule_type::EXECUTABLE:
    {
      // Need a command in context
      if (!ctx.command_name)
        return false;

      // A rule that pins a cwd names a binary by location, so it also
      // matches path-qualified spellings of that binary: either the
      // command name matches and it runs in the constrained directory,
      // or the resolved executable is exactly <cwd>/<command>.
      if (rule.cwd)
      {
        bool name_and_cwd = command_matches(rule.command, *ctx.command_name) && cwd_matches(rule.cwd, ctx.normalized_cwd);
        bool path_matches_rule = ctx.resolved_command_path && executable_path_matches(rule, *ctx.resolved_command_path);
        if (!name_and_cwd && !path_matches_rule)
        {
          return false;
        }
      }
      else if (!command_matches(rule.command, *ctx.command_name))
      {
        return false;
      }

      // Check args if specified in rule
      if (!rule.args.empty())
      {
        if (!ctx.command_args)
          return false;
        if (!args_match(rule.args, *ctx.command_args))
        {
          return false;
        }
      }

      return true;
    }

    case rule_type::UNKNOWN:
    default:
      return false;
    }
  }

  bool policy_engine::path_matches(
    const std::string& rule_path,
    const std::string& test_path,
    bool negated,
    const std::vector<std::string>& exclusions
  ) const
  {
    // Both paths should already be normalized at this point
    std::string norm_rule = rule_path;
    std::string norm_test = test_path;

    // Ensure both end with separator for directory matching
    if (!norm_rule.empty() && norm_rule.back() != '/')
    {
      norm_rule += '/';
    }

    // Check if test path starts with rule path
    bool matches = norm_test.starts_with(norm_rule);

    if (negated)
    {
      matches = !matches;
    }

    // Check exclusions
    if (matches && !exclusions.empty())
    {
      if (is_excluded(norm_test, exclusions))
      {
        return false;
      }
    }

    return matches;
  }

  bool policy_engine::is_excluded(const std::string& path, const std::vector<std::string>& exclusions) const
  {
    for (const auto& excl : exclusions)
    {
      std::string norm_excl = normalize_path(excl);
      if (!norm_excl.empty() && norm_excl.back() != '/')
      {
        norm_excl += '/';
      }

      if (path.starts_with(norm_excl) || path.starts_with(norm_excl.substr(0, norm_excl.size() - 1)))
      {
        return true;
      }

      // Also check for glob pattern matching
      if (excl.find('*') != std::string::npos || excl.find('?') != std::string::npos)
      {
        std::string pattern = glob_to_regex(excl);
        try
        {
          const std::regex& re = get_cached_regex(pattern);
          if (std::regex_match(path, re))
          {
            return true;
          }
        }
        catch (...)
        {
          // If regex compilation fails, skip
        }
      }
    }
    return false;
  }

  std::string policy_engine::generate_rule_id(const policy_rule& rule) const
  {
    std::ostringstream oss;
    oss << (rule.action == rule_action::ALLOW ? "allow" : "require") << ":" << static_cast<int>(rule.type) << ":"
        << rule.tool_name;

    switch (rule.type)
    {
    case rule_type::DIRECTORY:
      oss << ":dir=" << rule.path;
      if (rule.path_negated)
        oss << "!";
      if (!rule.exclusions.empty())
      {
        oss << ",exclude=";
        for (size_t i = 0; i < rule.exclusions.size(); ++i)
        {
          if (i > 0)
            oss << ",";
          oss << rule.exclusions[i];
        }
      }
      break;
    case rule_type::PATH_PREFIX:
      oss << ":prefix=" << rule.path_prefix;
      break;
    case rule_type::EXECUTABLE:
      oss << ":command=" << rule.command;
      if (!rule.args.empty())
      {
        oss << ":args=";
        for (size_t i = 0; i < rule.args.size(); ++i)
        {
          if (i > 0)
            oss << ",";
          oss << rule.args[i];
        }
      }
      if (rule.cwd)
      {
        oss << ":cwd=" << *rule.cwd;
      }
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
    rejection_messages_.clear();
    regex_cache_.clear();
  }

} // namespace mistral::agent
