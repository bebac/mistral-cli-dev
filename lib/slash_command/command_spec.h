#ifndef MISTRAL_SLASH_COMMAND_COMMAND_SPEC_H
#define MISTRAL_SLASH_COMMAND_COMMAND_SPEC_H

#include <functional>
#include <string>
#include <vector>

namespace mistral::slash_command
{
  /**
   * @brief A single completion candidate for one argument position.
   */
  struct completion_item
  {
    std::string text;        ///< The token to insert.
    std::string description; ///< Optional one-line description.
  };

  /**
   * @brief One positional argument of a command (or subcommand).
   */
  struct arg_spec
  {
    std::string name;        ///< Placeholder name, e.g. "server".
    std::string description; ///< Optional one-line description.
    bool required = false;   ///< Must be supplied for the command to run.
    bool variadic = false;   ///< Consumes the remaining tokens.
    /// Candidates for the given (partial) token. Unset => free-form argument.
    std::function<std::vector<completion_item>(const std::string& prefix)> completer;
  };

  /**
   * @brief Result of parsing a command line against a command_spec.
   */
  struct invocation
  {
    std::vector<std::string> path; ///< Command name followed by subcommand names.
    std::vector<std::string> args; ///< Positional argument tokens.
    std::string error;             ///< Non-empty when the line does not parse.

    bool ok() const
    {
      return error.empty();
    }
  };

  /**
   * @brief Completion state for a command line.
   */
  struct completion_state
  {
    std::vector<completion_item> items; ///< Candidates for the token being typed.
    bool complete = false;              ///< The line can be executed as typed.
    bool command_names = false;         ///< Items are command names (display with '/').
  };

  /**
   * @brief Declarative description of a slash command's grammar.
   *
   * A spec is pure data: it has no UI or coroutine dependencies, so the TUI
   * popup, the isocline completer and the command executor can all share one
   * description of what a command accepts.
   */
  struct command_spec
  {
    std::string name;                      ///< Command name, without the leading '/'.
    std::string summary;                   ///< One-line description.
    std::vector<arg_spec> args;            ///< Positional arguments at this node.
    std::vector<command_spec> subcommands; ///< Nested subcommands.

    /**
     * @brief Parse `args` (the text after the command name) against this spec.
     *
     * Only subcommand mismatches are reported: argument counts are left to
     * the command itself, so that commands keep control of their own error
     * messages.
     */
    invocation parse(const std::string& args) const;

    /**
     * @brief Candidates for the token being typed in `args`.
     *
     * A line is complete when every required argument is satisfied and the
     * token being typed is empty, already an exact candidate, or free-form
     * (no candidates at all). An empty token means "not typing anything", so
     * the popup acts as a hint and Enter runs the line as typed.
     */
    completion_state complete(const std::string& args) const;

    /**
     * @brief Render a usage line, e.g. "mcp [connect [server]|tools]".
     */
    std::string usage() const;
  };
} // namespace mistral::slash_command

#endif // MISTRAL_SLASH_COMMAND_COMMAND_SPEC_H
