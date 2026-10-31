#include "agent/policy_engine.h"
#include "common/executable_finder.h"
#include "common/string_utils.h"
#include "mcp/mcp_manager.h"

#include <algorithm>
#include <array>
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

    // Append a trailing separator so prefix comparisons respect
    // directory boundaries: "build" must not match "build-other".
    std::string with_trailing_slash(std::string path)
    {
      if (!path.empty() && path.back() != '/')
      {
        path += '/';
      }
      return path;
    }
  } // namespace

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
        if (rule.type == rule_type::UNKNOWN)
        {
          warnings_.push_back(std::format("ignoring policy rule with unknown type: {}", rule_json.dump()));
          continue;
        }
        push_rule(std::move(rule));
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
        if (rule.type == rule_type::UNKNOWN)
        {
          warnings_.push_back(std::format("ignoring policy rule with unknown type: {}", rule_json.dump()));
          continue;
        }
        push_rule(std::move(rule));
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
        set_rule_path(rule, rule_json["path"].get<std::string>());
        if (rule_json.contains("exclude"))
        {
          std::vector<std::string> exclude;
          for (const auto& excl : rule_json["exclude"])
          {
            exclude.push_back(excl.get<std::string>());
          }
          add_exclusions(rule, exclude);
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
      push_rule(std::move(rule));
    }
  }

  void policy_engine::push_rule(policy_rule rule)
  {
    if (rule.id.empty())
    {
      rule.id = generate_rule_id(rule);
    }
    rules_.push_back(std::move(rule));
  }

  void policy_engine::set_rule_path(policy_rule& rule, const std::string& path) const
  {
    rule.path_negated = path.starts_with("!");
    rule.path = normalize_path(rule.path_negated ? path.substr(1) : path);
  }

  void policy_engine::add_exclusions(policy_rule& rule, const std::vector<std::string>& exclude) const
  {
    // Store exclusions normalized, so matching does not have to touch
    // the filesystem on every evaluation.
    for (const auto& entry : exclude)
    {
      rule.exclusions.push_back(normalize_path((fs::path(rule.path) / entry).string()));
    }
  }

  void policy_engine::add_directory_rule(
    rule_action action,
    const std::string& tool_name,
    const std::string& path,
    const std::vector<std::string>& exclude
  )
  {
    policy_rule rule{.action = action, .type = rule_type::DIRECTORY, .tool_name = tool_name};
    set_rule_path(rule, path);
    add_exclusions(rule, exclude);
    push_rule(std::move(rule));
  }

  void policy_engine::allow_tool(const std::string& tool_name)
  {
    push_rule(policy_rule{.action = rule_action::ALLOW, .type = rule_type::TOOL, .tool_name = tool_name});
  }

  void policy_engine::allow_directory(
    const std::string& tool_name,
    const std::string& path,
    const std::vector<std::string>& exclude
  )
  {
    add_directory_rule(rule_action::ALLOW, tool_name, path, exclude);
  }

  void policy_engine::require_approval_tool(const std::string& tool_name)
  {
    push_rule(policy_rule{.action = rule_action::REQUIRE_APPROVAL, .type = rule_type::TOOL, .tool_name = tool_name});
  }

  void policy_engine::require_approval_directory(const std::string& tool_name, const std::string& path_pattern)
  {
    add_directory_rule(rule_action::REQUIRE_APPROVAL, tool_name, path_pattern, {});
  }

  void policy_engine::require_approval_path_prefix(const std::string& tool_name, const std::string& prefix)
  {
    push_rule(policy_rule{
      .action = rule_action::REQUIRE_APPROVAL,
      .type = rule_type::PATH_PREFIX,
      .tool_name = tool_name,
      .path_prefix = normalize_path(prefix)
    });
  }

  void policy_engine::add_executable_rule(
    rule_action action,
    const std::string& tool_name,
    const std::string& command,
    const std::vector<std::string>& args,
    const std::optional<std::string>& cwd
  )
  {
    policy_rule rule{
      .action = action, .type = rule_type::EXECUTABLE, .tool_name = tool_name, .command = command, .args = args
    };
    if (cwd)
    {
      rule.cwd = normalize_path(*cwd);
    }
    push_rule(std::move(rule));
  }

  void policy_engine::allow_executable(
    const std::string& tool_name,
    const std::string& command,
    const std::vector<std::string>& args,
    const std::optional<std::string>& cwd
  )
  {
    add_executable_rule(rule_action::ALLOW, tool_name, command, args, cwd);
  }

  void policy_engine::require_approval_executable(
    const std::string& tool_name,
    const std::string& command,
    const std::vector<std::string>& args,
    const std::optional<std::string>& cwd
  )
  {
    add_executable_rule(rule_action::REQUIRE_APPROVAL, tool_name, command, args, cwd);
  }

  void policy_engine::set_default_policy(policy_decision default_policy)
  {
    default_policy_ = default_policy;
  }

  void policy_engine::set_rejection_message(const std::string& rule_id, const std::string& message)
  {
    rejection_messages_[rule_id] = message;
  }

  policy_result policy_engine::make_result(const policy_rule& rule) const
  {
    if (rule.action == rule_action::ALLOW)
    {
      return policy_result{policy_decision::ALLOW, std::format("Allowed by rule: {}", rule.id), std::nullopt};
    }

    std::optional<std::string> msg = rule.custom_message;
    if (!msg && !rule.id.empty())
    {
      auto it = rejection_messages_.find(rule.id);
      if (it != rejection_messages_.end())
      {
        msg = it->second;
      }
    }

    return policy_result{
      policy_decision::REQUIRE_APPROVAL, std::format("Requires approval by rule: {}", rule.id), msg
    };
  }

  policy_result policy_engine::evaluate(const policy_context& ctx) const
  {
    policy_context normalized_ctx = normalize_context(ctx);

    // Priority order: DIRECTORY > PATH_PREFIX > EXECUTABLE > TOOL.
    // Within each type, check in reverse order (most recently added first).
    static constexpr std::array<rule_type, 4> priority{
      rule_type::DIRECTORY, rule_type::PATH_PREFIX, rule_type::EXECUTABLE, rule_type::TOOL
    };

    for (rule_type type : priority)
    {
      for (const auto& rule : rules_ | std::views::reverse)
      {
        if (rule.type == type && rule_matches(rule, normalized_ctx))
        {
          return make_result(rule);
        }
      }
    }

    // Apply the default policy.
    switch (default_policy_)
    {
    case policy_decision::ALLOW:
      return policy_result{policy_decision::ALLOW, "Default policy: allow", std::nullopt};
    case policy_decision::REQUIRE_APPROVAL:
      return policy_result{policy_decision::REQUIRE_APPROVAL, "Default policy: require approval", std::nullopt};
    case policy_decision::DENY:
    default:
      return policy_result{policy_decision::DENY, "Default policy: deny", std::nullopt};
    }
  }

  std::string policy_engine::normalize_path(const std::string& path)
  {
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    if (ec)
    {
      return path;
    }

    // weakly_canonical resolves ".." and symlinks where the path exists.
    fs::path normalized = fs::weakly_canonical(absolute, ec);
    if (ec)
    {
      return absolute.lexically_normal().string();
    }
    return normalized.string();
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

  bool policy_engine::glob_match(const std::string& pattern, const std::string& text) const
  {
    // No wildcards: a plain comparison, no regex needed.
    if (pattern.find_first_of("*?") == std::string::npos)
    {
      return pattern == text;
    }

    try
    {
      return std::regex_match(text, get_cached_regex(glob_to_regex(pattern)));
    }
    catch (const std::regex_error&)
    {
      // Pathological patterns can fail at match time; fall back to an
      // exact comparison rather than propagating.
      return pattern == text;
    }
  }

  bool policy_engine::command_matches(const std::string& rule_command, const std::string& test_command) const
  {
    return glob_match(rule_command, test_command);
  }

  bool policy_engine::args_match(
    const std::vector<std::string>& rule_args,
    const std::vector<std::string>& test_args
  ) const
  {
    // "List match": any test arg matching any rule arg is enough.
    return std::ranges::any_of(test_args, [&](const std::string& test_arg) {
      return std::ranges::any_of(rule_args, [&](const std::string& rule_arg) {
        return glob_match(rule_arg, test_arg);
      });
    });
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

    // Directory-boundary prefix match: "build" matches "build" and
    // "build/sub", but not "build-other".
    return with_trailing_slash(*test_cwd).starts_with(with_trailing_slash(*rule_cwd));
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
        set_rule_path(rule, dir_part.substr(4));
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
        add_exclusions(rule, split_comma(parts[i].substr(8)));
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
    // Both paths are already normalized at this point.
    bool matches = test_path.starts_with(with_trailing_slash(rule_path));

    if (negated)
    {
      matches = !matches;
    }

    // Check exclusions
    if (matches && !exclusions.empty())
    {
      if (is_excluded(test_path, exclusions))
      {
        return false;
      }
    }

    return matches;
  }

  bool policy_engine::is_excluded(const std::string& path, const std::vector<std::string>& exclusions) const
  {
    // Exclusions are stored normalized by add_exclusions().
    for (const auto& excl : exclusions)
    {
      // The excluded directory itself, or anything below it.
      if (path.starts_with(excl))
      {
        return true;
      }

      // Glob patterns are matched against the whole path.
      if (glob_match(excl, path))
      {
        return true;
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
    warnings_.clear();
    regex_cache_.clear();
  }

} // namespace mistral::agent
