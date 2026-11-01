#include "slash_command/command_spec.h"

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

  completion_state command_spec::complete(const std::string& args) const
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
      if (arg.completer)
      {
        for (auto& item : arg.completer(current))
        {
          if (starts_with(item.text, current))
          {
            out.items.push_back(std::move(item));
          }
        }
      }
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
} // namespace mistral::slash_command
