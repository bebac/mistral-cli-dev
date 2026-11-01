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

  command_catalog slash_command_registry::build_catalog() const
  {
    command_catalog catalog;

    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& [name, cmd] : commands_)
    {
      catalog.commands.push_back(cmd->spec());
      catalog.context.command_names.push_back({name, cmd->description()});
    }
    return catalog;
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
