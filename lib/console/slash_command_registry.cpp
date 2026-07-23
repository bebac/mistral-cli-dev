#include "console/slash_command_registry.h"

#include <stdexcept>

namespace mistral::console
{
  slash_command_registry &slash_command_registry::instance()
  {
    static slash_command_registry registry;
    return registry;
  }

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

  const slash_command *slash_command_registry::get_command(const std::string &name) const
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
    for (const auto &pair : commands_)
    {
      names.push_back(pair.first);
    }
    return names;
  }

  bool slash_command_registry::needs_agent_execution(const std::string &name) const
  {
    const slash_command *cmd = get_command(name);
    if (!cmd)
    {
      return false;
    }
    return cmd->context() == slash_command::execution_context::AGENT;
  }

  std::string slash_command_registry::get_usage(const std::string &name) const
  {
    const slash_command *cmd = get_command(name);
    if (!cmd)
    {
      return "";
    }
    return cmd->usage();
  }

  bool slash_command_registry::has_command(const std::string &name) const
  {
    std::lock_guard<std::mutex> lock(mtx_);
    return commands_.find(name) != commands_.end();
  }
} // namespace mistral::console