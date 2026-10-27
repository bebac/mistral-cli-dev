#ifndef MISTRAL_SLASH_COMMAND_H
#define MISTRAL_SLASH_COMMAND_H

#include <dripcore/awaitable.h>
#include <dripcore/task.h>
#include <string>

namespace mistral::agent
{
  class agent_system;
}

namespace mistral::slash_command
{
  // Forward declaration for request struct
  struct slash_command_request;

  /**
   * @brief Base class for all slash commands.
   *
   * Slash commands are prefixed with '/' and provide special functionality
   * beyond regular chat input. All commands execute in a dedicated executor
   * task with full access to the agent system.
   */
  class slash_command
  {
  public:
    virtual ~slash_command() = default;

    // --- Metadata ---

    /**
     * @brief Get the command name (without leading '/').
     * @return The command name.
     */
    virtual std::string name() const = 0;

    /**
     * @brief Get a brief description of the command.
     * @return The command description.
     */
    virtual std::string description() const = 0;

    /**
     * @brief Get the usage string for the command.
     * @return The usage string (default: just the command name).
     */
    virtual std::string usage() const
    {
      return name();
    }

    // --- Execution ---

    /**
     * @brief Execute the command in the slash command executor task.
     *
     * Can block (await) on dripcore operations and has full access to the
     * agent system.
     *
     * @param task The task context for spawning sub-tasks.
     * @param args The command arguments (everything after the command name).
     * @param agent_sys Reference to the agent system singleton.
     * @return The response string to display to the user.
     */
    virtual dripcore::awaitable<std::string> execute(
      dripcore::task& task,
      const std::string& args,
      agent::agent_system& agent_sys
    ) = 0;
  };

  /**
   * @brief Request structure for slash command execution.
   *
   * Used to pass command requests from the input thread to the executor task.
   */
  struct slash_command_request
  {
    std::string command_name; ///< The name of the command to execute.
    std::string args;         ///< The arguments passed to the command.
  };
} // namespace mistral::slash_command

#endif // MISTRAL_SLASH_COMMAND_H
