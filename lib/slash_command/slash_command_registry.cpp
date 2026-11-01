#include "slash_command/slash_command_registry.h"

#include <cctype>
#include <stdexcept>

namespace mistral::slash_command
{
  namespace
  {
    // A command line split into its command name and the remaining text.
    struct split_line
    {
      std::string command;
      std::string rest;
      bool has_space = false;
    };

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
  } // namespace

  void slash_command_registry::register_command(std::unique_ptr<slash_command> cmd)
  {
    if (!cmd)
    {
      return;
    }

    std::lock_guard<std::mutex> lock(mtx_);
    std::string name = cmd->name();
    if (name.empty())
    {
      throw std::invalid_argument("Command name cannot be empty");
    }
    commands_[std::move(name)] = std::move(cmd);
  }

  const slash_command* slash_command_registry::get_command(const std::string& name) const
  {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = commands_.find(name);
    if (it == commands_.end())
    {
      return nullptr;
    }
    return it->second.get();
  }

  std::vector<std::string> slash_command_registry::get_command_names() const
  {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<std::string> names;
    names.reserve(commands_.size());
    for (const auto& pair : commands_)
    {
      names.push_back(pair.first);
    }
    return names;
  }

  completion_state slash_command_registry::complete(const std::string& line) const
  {
    completion_state out;
    const split_line split = split_command(line);

    if (!split.has_space)
    {
      // Completing the command name itself.
      out.command_names = true;
      for (const auto& name : get_command_names())
      {
        if (!starts_with_ci(name, split.command))
        {
          continue;
        }
        const slash_command* cmd = get_command(name);
        out.items.push_back({name, cmd ? cmd->description() : ""});
      }

      // An exact command name is complete when the command's own spec is
      // satisfied, so "/mcp" runs while "/tool" waits for its arguments.
      for (const auto& item : out.items)
      {
        if (item.text == split.command)
        {
          const slash_command* cmd = get_command(item.text);
          out.complete = cmd ? cmd->spec().complete("").complete : true;
          break;
        }
      }
      return out;
    }

    const slash_command* cmd = get_command(split.command);
    if (!cmd)
    {
      return out;
    }
    return cmd->spec().complete(split.rest);
  }

  invocation slash_command_registry::parse_line(const std::string& line) const
  {
    const split_line split = split_command(line);
    const slash_command* cmd = get_command(split.command);
    if (!cmd)
    {
      invocation out;
      out.error = "unknown command '" + split.command + "'";
      return out;
    }
    return cmd->spec().parse(split.rest);
  }

  std::string slash_command_registry::validate(const std::string& command, const std::string& args) const
  {
    const slash_command* cmd = get_command(command);
    if (!cmd)
    {
      return "Unknown command: /" + command;
    }

    const invocation inv = cmd->spec().parse(args);
    if (!inv.ok())
    {
      return "Error: " + inv.error + "\nusage: /" + cmd->usage();
    }
    return "";
  }

  std::string slash_command_registry::get_usage(const std::string& name) const
  {
    const slash_command* cmd = get_command(name);
    if (!cmd)
    {
      return "";
    }
    return cmd->usage();
  }

  bool slash_command_registry::has_command(const std::string& name) const
  {
    std::lock_guard<std::mutex> lock(mtx_);
    return commands_.contains(name);
  }
} // namespace mistral::slash_command
