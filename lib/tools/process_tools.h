#ifndef MISTRAL_PROCESS_TOOLS_H
#define MISTRAL_PROCESS_TOOLS_H

#include "tools/exec_tools.h" // for exec_request (shared command/args/cwd shape)

#include <dripcore/awaitable.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mistral::common
{
  class process_manager;
}

namespace mistral::tools
{
  using json = nlohmann::ordered_json;

  // The process_* tool family: long-running background processes and
  // REPL sessions (dev servers, lldb/gdb, ...) that outlive a single
  // tool call. All state lives in the process_manager passed to the
  // constructor (dependency injection, same pattern as patchset_tools
  // taking patchset_store&); the tools themselves are stateless.
  //
  // Like exec, commands run via execvp() with no shell: `command` is a
  // single executable name and each argument a separate array element.
  class process_tools
  {
  public:
    explicit process_tools(common::process_manager& mgr)
      : mgr_(mgr)
    {
    }

    // Start a background process and return immediately with its
    // session id. When wait_for (a regex) is non-empty, block until the
    // process output matches it, the process exits, or
    // wait_timeout_seconds elapse (default 60) - e.g. to wait for a
    // dev server to print its "ready" line. env entries are "KEY=VALUE"
    // strings applied over the inherited environment. name optionally
    // sets the session id (letters, digits, '.', '_', '-'; unique).
    dripcore::awaitable<json> process_start(
      std::string command,
      std::vector<std::string> args,
      std::string cwd,
      std::vector<std::string> env,
      std::string name,
      std::string wait_for,
      int wait_timeout_seconds
    );

    // Write input verbatim to the session's stdin (include a trailing
    // newline for line-based REPLs). Blocks only while the pipe is
    // full; fails when the process has exited.
    dripcore::awaitable<json> process_write(std::string id, std::string input);

    // Output produced since byte offset `since` (0 = from the start;
    // pass next_offset from the previous read for only new output),
    // capped at max_bytes per call. stdout+stderr merged; the session
    // retains the last 256 KiB, older output is reported via truncated.
    json process_read(std::string id, int64_t since, int max_bytes);

    // All sessions with pid, running/exited, exit_code, uptime.
    json process_list();

    // SIGKILL the session's whole process group and reap it. The
    // session stays listed; its final output remains readable.
    json process_kill(std::string id);

    // Parse the command/args/cwd fields of a process_start call (same
    // shape as exec) for the precheck hook. Returns nullopt when
    // malformed, so the caller falls back to the dispatcher's errors.
    static std::optional<exec_request> parse_request(const json& args);

  private:
    common::process_manager& mgr_;
  };
} // namespace mistral::tools

#endif // MISTRAL_PROCESS_TOOLS_H
