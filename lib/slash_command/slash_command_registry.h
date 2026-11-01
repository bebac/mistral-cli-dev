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

    // Build the static completion catalog: every command's spec plus the
    // command names. The caller owns the result and fills the dynamic part
    // of the context (MCP servers, agent tools) itself.
    //
    // spec() and description() are called with the registry lock released, so
    // a command may query the registry from them. Registration must not race
    // with this call (commands are registered at startup).
    command_catalog build_catalog() const;

    // Parse a command line (the text after the leading '/') against the
    // command's spec.
    invocation parse_line(const std::string& line) const;

    // Validate a command invocation. Returns an empty string when the
    // invocation is well formed, otherwise a user-facing error message.
    std::string validate(const std::string& command, const std::string& args) const;

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
