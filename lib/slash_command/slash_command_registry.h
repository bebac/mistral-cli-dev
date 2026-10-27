#ifndef MISTRAL_SLASH_COMMAND_REGISTRY_H
#define MISTRAL_SLASH_COMMAND_REGISTRY_H

#include "slash_command/slash_command.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mistral::slash_command
{
  /**
   * @brief Registry for managing slash commands.
   *
   * This class provides thread-safe registration and lookup of slash commands.
   * Commands are registered at startup and can be looked up by name for execution.
   * Owned by agent_system and shared with console_task and
   * slash_command_executor_task via shared_ptr.
   */
  class slash_command_registry
  {
  public:
    slash_command_registry() = default;

    // Register a command (typically called at startup)
    void register_command(std::unique_ptr<slash_command> cmd);

    // Lookup by name
    const slash_command* get_command(const std::string& name) const;

    // Get all command names (for completion)
    std::vector<std::string> get_command_names() const;

    // Get usage string for help
    std::string get_usage(const std::string& name) const;

    // Check if a command exists
    bool has_command(const std::string& name) const;

  private:
    std::map<std::string, std::unique_ptr<slash_command>> commands_;
    mutable std::mutex mtx_;
  };
} // namespace mistral::slash_command

#endif // MISTRAL_SLASH_COMMAND_REGISTRY_H
