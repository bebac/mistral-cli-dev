#ifndef MISTRAL_SLASH_COMMAND_COMMAND_SPEC_H
#define MISTRAL_SLASH_COMMAND_COMMAND_SPEC_H

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
   * @brief Where an argument's completion candidates come from.
   *
   * Declarative on purpose: a command_spec stays pure data (no lambdas, no
   * singleton access), so a frontend can own a copy and resolve candidates
   * against data it already holds.
   */
  enum class candidate_source
  {
    none,          ///< Free-form argument.
    literal,       ///< arg_spec::literals.
    command_names, ///< Every slash command name.
    mcp_servers,   ///< Configured MCP servers.
    agent_tools,   ///< Tools available to the current agent.
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
    candidate_source source = candidate_source::none;
    std::vector<completion_item> literals; ///< Used when source == literal.
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
   * @brief Dynamic completion data, owned by the frontend.
   *
   * command_names is filled when the catalog is built; mcp_servers and
   * agent_tools are refreshed when the agent's tool catalog changes.
   */
  struct completion_context
  {
    std::vector<completion_item> command_names;
    std::vector<completion_item> mcp_servers;
    std::vector<completion_item> agent_tools;
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
    completion_state complete(const std::string& args, const completion_context& context) const;

    /**
     * @brief Render a usage line, e.g. "mcp [connect [server]|tools]".
     */
    std::string usage() const;
  };

  /**
   * @brief Everything a frontend needs to complete a slash command line.
   *
   * Built once from the registry (static part) and updated in place when the
   * dynamic part changes. Owned by the frontend, so completion needs no
   * registry lookup, no locks and no cross-thread calls.
   */
  struct command_catalog
  {
    std::vector<command_spec> commands;
    completion_context context;
  };

  /**
   * @brief Complete `line` (the text after the leading '/') against a catalog.
   *
   * Pure function: no registry, no locks, no singletons.
   */
  completion_state complete_line(const command_catalog& catalog, const std::string& line);
} // namespace mistral::slash_command

#endif // MISTRAL_SLASH_COMMAND_COMMAND_SPEC_H
