#ifndef MISTRAL_SLASH_COMMAND_H
#define MISTRAL_SLASH_COMMAND_H

#include <string>
#include <dripcore/task.h>
#include <dripcore/awaitable.h>

namespace mistral::agent
{
  class agent_system;
}

namespace mistral::console
{
  // Forward declaration for request struct
  struct slash_command_request;

  /**
   * @brief Base class for all slash commands.
   *
   * Slash commands are prefixed with '/' and provide special functionality
   * beyond regular chat input. Commands can execute either immediately in
   * the input thread (for simple operations) or in a dedicated executor task
   * (for operations requiring agent state access).
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

    // --- Execution Context ---

    /**
     * @brief Execution context for slash commands.
     */
    enum class execution_context
    {
      IMMEDIATE, // Execute synchronously in input thread (no agent access)
      AGENT      // Execute in slash_cmd_executor_task (full agent access)
    };

    /**
     * @brief Get the execution context for this command.
     * @return The execution context.
     */
    virtual execution_context context() const = 0;

    // --- IMMEDIATE Commands ---

    /**
     * @brief Execute the command immediately in the input thread.
     *
     * This method is called for IMMEDIATE context commands.
     * Must be thread-safe and non-blocking.
     *
     * @param args The command arguments (everything after the command name).
     * @return The response string to display to the user.
     * @throws std::logic_error if the command doesn't support IMMEDIATE execution.
     */
    virtual std::string execute_immediate(const std::string &args)
    {
      throw std::logic_error("Command does not support IMMEDIATE execution");
    }

    // --- AGENT Commands ---

    /**
     * @brief Execute the command in the slash command executor task.
     *
     * This method is called for AGENT context commands.
     * Can block (await) on other dripcore operations.
     *
     * @param task The task context for spawning sub-tasks.
     * @param args The command arguments (everything after the command name).
     * @param agent_sys Reference to the agent system singleton.
     * @return The response string to display to the user.
     * @throws std::logic_error if the command doesn't support AGENT execution.
     */
    virtual dripcore::awaitable<std::string> execute_agent(
      dripcore::task& task,
      const std::string &args,
      agent::agent_system &agent_sys
    )
    {
      throw std::logic_error("Command does not support AGENT execution");
    }
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
} // namespace mistral::console

#endif // MISTRAL_SLASH_COMMAND_H
