#include "slash_command/command_spec.h"

#include <cctype>
#include <utility>

namespace mistral::slash_command
{
  namespace
  {
    // A line split on whitespace. `ends_with_space` distinguishes "mcp" from
    // "mcp ": the latter means the user is starting a new token.
    struct tokenized_line
    {
      std::vector<std::string> tokens;
      bool ends_with_space = false;
    };

    tokenized_line tokenize(const std::string& line)
    {
      tokenized_line out;
      std::string current;
      for (char c : line)
      {
        if (c == ' ' || c == '\t')
        {
          if (!current.empty())
          {
            out.tokens.push_back(std::move(current));
            current.clear();
          }
        }
        else
        {
          current.push_back(c);
        }
      }
      if (current.empty())
      {
        out.ends_with_space = true;
      }
      else
      {
        out.tokens.push_back(std::move(current));
      }
      return out;
    }

    bool starts_with(const std::string& text, const std::string& prefix)
    {
      return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
    }

    bool starts_with_ci(const std::string& text, const std::string& prefix)
    {
      if (prefix.size() > text.size())
      {
        return false;
      }
      for (size_t i = 0; i < prefix.size(); ++i)
      {
        if (
          std::tolower(static_cast<unsigned char>(text[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))
        )
        {
          return false;
        }
      }
      return true;
    }

    const command_spec* find_command(const command_catalog& catalog, const std::string& name)
    {
      for (const auto& spec : catalog.commands)
      {
        if (spec.name == name)
        {
          return &spec;
        }
      }
      return nullptr;
    }

    const command_spec* find_subcommand(const command_spec& spec, const std::string& name)
    {
      for (const auto& sub : spec.subcommands)
      {
        if (sub.name == name)
        {
          return &sub;
        }
      }
      return nullptr;
    }

    std::vector<completion_item> matching_subcommands(const command_spec& spec, const std::string& prefix)
    {
      std::vector<completion_item> items;
      for (const auto& sub : spec.subcommands)
      {
        if (starts_with(sub.name, prefix))
        {
          items.push_back({sub.name, sub.summary});
        }
      }
      return items;
    }

    // Candidates for an argument, resolved from the declarative source.
    const std::vector<completion_item>& source_items(
      const arg_spec& arg,
      const completion_context& context
    )
    {
      switch (arg.source)
      {
      case candidate_source::literal:
        return arg.literals;
      case candidate_source::command_names:
        return context.command_names;
      case candidate_source::mcp_servers:
        return context.mcp_servers;
      case candidate_source::agent_tools:
        return context.agent_tools;
      case candidate_source::model_ids:
        return context.model_ids;
      case candidate_source::none:
        break;
      }

      static const std::vector<completion_item> empty;
      return empty;
    }

    // Result of descending the subcommand chain.
    struct descent
    {
      const command_spec* node = nullptr; ///< Resolved node, null on error.
      std::vector<std::string> path;      ///< Names from the root to the node.
      std::vector<std::string> rest;      ///< Tokens left after the node.
      std::string partial_subcommand;     ///< Subcommand token being typed.
      std::string error;                  ///< Subcommand mismatch.
    };

    descent descend(const command_spec& root, const tokenized_line& line)
    {
      descent out;
      out.node = &root;
      out.path.push_back(root.name);

      size_t i = 0;
      while (i < line.tokens.size() && !out.node->subcommands.empty())
      {
        const std::string& token = line.tokens[i];
        if (const command_spec* sub = find_subcommand(*out.node, token))
        {
          out.node = sub;
          out.path.push_back(sub->name);
          ++i;
          continue;
        }

        // Not an exact subcommand. If it is the token currently being typed,
        // leave it for the caller to complete; otherwise it is a typo.
        const bool is_last = (i + 1 == line.tokens.size());
        if (is_last && !line.ends_with_space)
        {
          out.partial_subcommand = token;
          out.rest.assign(line.tokens.begin() + i, line.tokens.end());
          return out;
        }

        out.error = "unknown subcommand '" + token + "'";
        out.node = nullptr;
        return out;
      }

      out.rest.assign(line.tokens.begin() + i, line.tokens.end());
      return out;
    }

    std::string args_usage(const command_spec& spec)
    {
      std::string out;
      for (const auto& arg : spec.args)
      {
        out += arg.required ? " <" : " [";
        out += arg.name;
        out += arg.required ? ">" : "]";
      }
      return out;
    }

    std::string usage_inline(const command_spec& spec)
    {
      std::string out = spec.name + args_usage(spec);
      if (!spec.subcommands.empty())
      {
        out += " [";
        for (size_t i = 0; i < spec.subcommands.size(); ++i)
        {
          if (i > 0)
          {
            out += "|";
          }
          out += usage_inline(spec.subcommands[i]);
        }
        out += "]";
      }
      return out;
    }
  } // namespace

  split_line split_command(const std::string& line)
  {
    split_line out;
    const size_t space = line.find_first_of(" \t");
    if (space == std::string::npos)
    {
      out.command = line;
      return out;
    }
    out.command = line.substr(0, space);
    out.rest = line.substr(space + 1);
    out.has_space = true;
    return out;
  }

  invocation command_spec::parse(const std::string& args) const
  {
    invocation out;
    const tokenized_line line = tokenize(args);
    const descent walk = descend(*this, line);

    if (!walk.node)
    {
      out.error = walk.error;
      return out;
    }
    if (!walk.partial_subcommand.empty())
    {
      out.error = "unknown subcommand '" + walk.partial_subcommand + "'";
      return out;
    }

    out.path = walk.path;
    out.args = walk.rest;
    return out;
  }

  completion_state command_spec::complete(const std::string& args, const completion_context& context) const
  {
    completion_state out;
    const tokenized_line line = tokenize(args);
    const descent walk = descend(*this, line);

    if (!walk.node)
    {
      // A typo: nothing to suggest, but let the line submit so that the
      // executor can report the error.
      out.complete = true;
      return out;
    }

    if (!walk.partial_subcommand.empty())
    {
      out.items = matching_subcommands(*walk.node, walk.partial_subcommand);
      out.complete = out.items.empty();
      return out;
    }

    // Work out which argument is being typed.
    size_t arg_index = 0;
    std::string current;
    if (walk.rest.empty())
    {
      arg_index = 0;
    }
    else if (line.ends_with_space)
    {
      arg_index = walk.rest.size();
    }
    else
    {
      arg_index = walk.rest.size() - 1;
      current = walk.rest.back();
    }

    // Offer the subcommands when no subcommand has been typed yet.
    if (walk.rest.empty() && !walk.node->subcommands.empty())
    {
      out.items = matching_subcommands(*walk.node, "");
    }

    if (arg_index < walk.node->args.size())
    {
      const arg_spec& arg = walk.node->args[arg_index];
      size_t arg_items = 0;
      for (const auto& item : source_items(arg, context))
      {
        if (starts_with(item.text, current))
        {
          out.items.push_back(item);
          ++arg_items;
        }
      }

      // Candidates are the point of this argument: with an empty token,
      // Enter takes the highlighted one instead of running the line
      // without it.
      out.prefer_candidate = arg.prefer_candidate && current.empty() && arg_items > 0;
    }

    // Required arguments after the one being typed are unsatisfied. A
    // non-empty token means the current argument is being supplied.
    const size_t required_from = arg_index + (current.empty() ? 0 : 1);
    bool required_ok = true;
    for (size_t i = required_from; i < walk.node->args.size(); ++i)
    {
      if (walk.node->args[i].required)
      {
        required_ok = false;
        break;
      }
    }

    bool exact = false;
    for (const auto& item : out.items)
    {
      if (item.text == current)
      {
        exact = true;
        break;
      }
    }

    // A token that has no candidates at all is free-form, so it is complete
    // as soon as the required arguments are satisfied.
    out.complete = required_ok && (current.empty() || exact || out.items.empty());
    return out;
  }

  std::string command_spec::usage() const
  {
    return usage_inline(*this);
  }

  completion_state complete_line(const command_catalog& catalog, const std::string& line)
  {
    completion_state out;
    const split_line split = split_command(line);

    if (!split.has_space)
    {
      // Completing the command name itself.
      out.command_names = true;
      for (const auto& item : catalog.context.command_names)
      {
        if (starts_with_ci(item.text, split.command))
        {
          out.items.push_back(item);
        }
      }

      // An exact command name is complete when the command's own spec is
      // satisfied, so "/mcp" runs while "/tool" waits for its arguments.
      for (const auto& item : out.items)
      {
        if (item.text == split.command)
        {
          const command_spec* spec = find_command(catalog, item.text);
          out.complete = spec ? spec->complete("", catalog.context).complete : true;
          break;
        }
      }
      return out;
    }

    const command_spec* spec = find_command(catalog, split.command);
    if (!spec)
    {
      return out;
    }
    return spec->complete(split.rest, catalog.context);
  }
} // namespace mistral::slash_command
