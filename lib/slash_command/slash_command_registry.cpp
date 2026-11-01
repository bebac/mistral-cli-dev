#include "slash_command/slash_command_registry.h"

#include <stdexcept>
#include <utility>

namespace mistral::slash_command
{
  void slash_command_registry::register_command(std::unique_ptr<slash_command> cmd)
  {
    if (!cmd)
    {
      return;
    }

    // name() is command-supplied code: call it before taking the lock, for
    // the same reason build_catalog() does.
    std::string name = cmd->name();
    if (name.empty())
    {
      throw std::invalid_argument("Command name cannot be empty");
    }

    std::lock_guard<std::mutex> lock(mtx_);
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
    // Copy the map contents under the lock, then call the virtuals with the
    // lock released. spec() and description() are command-supplied code, and
    // a command that asked the registry for anything would deadlock on the
    // non-recursive mutex. The pointers stay valid because commands are
    // registered at startup only, which is the contract get_command() and
    // parse_line() already rely on.
    std::vector<std::pair<std::string, const slash_command*>> commands;
    {
      std::lock_guard<std::mutex> lock(mtx_);
      commands.reserve(commands_.size());
      for (const auto& [name, cmd] : commands_)
      {
        commands.emplace_back(name, cmd.get());
      }
    }

    command_catalog catalog;
    catalog.commands.reserve(commands.size());
    catalog.context.command_names.reserve(commands.size());
    for (const auto& [name, cmd] : commands)
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
