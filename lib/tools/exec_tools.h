#ifndef MISTRAL_EXEC_TOOLS_H
#define MISTRAL_EXEC_TOOLS_H

#include <dripcore/awaitable.h>
#include <dripcore/channel.h>
#include <dripcore/task.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mistral::tools
{
  using json = nlohmann::ordered_json;

  // The arguments of one exec call, as they appear in the tool-call JSON.
  struct exec_request
  {
    std::string command;
    std::vector<std::string> args;
    std::string cwd;
    int timeout_seconds = 30;
  };

  // Events published by exec_run_task while a command runs.
  struct exec_event
  {
    enum class type
    {
      output, // a chunk of stdout or stderr
      done    // final result; text holds the JSON payload
    };

    type kind = type::output;
    std::string text;
    bool is_stderr = false;
  };

  // Runs one command as a task and publishes its progress on a channel:
  // one `output` event per chunk read from the child, then exactly one
  // `done` event carrying the same JSON payload exec_tools::exec()
  // returns. The channel is closed when the task finishes, on every path
  // including cancellation and an escaping exception.
  //
  // Two independent stop signals, because they need different endings:
  //   - `cancel_requested` set: the user cancelled. The child's process
  //     group is killed and a final `done` event still carries the
  //     partial output, so the consumer records what was produced.
  //   - channel closed: the consumer is gone. The child is killed and
  //     nothing more is sent.
  class exec_run_task : public dripcore::task
  {
  public:
    exec_run_task(
      std::string command,
      std::vector<std::string> args,
      std::string cwd,
      int timeout_seconds,
      std::shared_ptr<dripcore::channel<exec_event>> out_ch,
      std::shared_ptr<std::atomic<bool>> cancel_requested
    );

  protected:
    dripcore::awaitable<> body() override;
    dripcore::awaitable<> teardown() override;

  private:
    std::string command_;
    std::vector<std::string> args_;
    std::string cwd_;
    int timeout_seconds_;
    std::shared_ptr<dripcore::channel<exec_event>> out_ch_;
    std::shared_ptr<std::atomic<bool>> cancel_requested_;
  };

  class exec_tools
  {
  public:
    // Execute an external command with controlled access.
    // Uses fork() + execvp() directly - no shell involved.
    // Returns JSON with exit code, stdout, stderr, and status.
    //
    // This is the non-streaming entry point used by the tool registry.
    // Agents that want live output run exec_run_task instead (see
    // agent_task_base::execute_tool).
    dripcore::awaitable<json> exec(
      std::string command,
      std::vector<std::string> args,
      std::string cwd = "",
      int timeout_seconds = 30
    );

    // Parse the exec tool's JSON arguments. Returns std::nullopt when they
    // are malformed (missing command, wrong types), so the caller can fall
    // back to the tool dispatcher's error reporting.
    static std::optional<exec_request> parse_request(const json& args);

    // Pre-check an exec tool call before it is executed (or approved).
    // Returns an error message if the command is empty, contains shell
    // syntax (execvp() is called directly with no shell), the working
    // directory does not exist, or the command does not exist (PATH
    // lookup, then <cwd>/<command> for a bare name, or - for explicit
    // paths - not an executable file). Returns std::nullopt when the
    // call looks valid.
    static std::optional<std::string> validate(
      const std::string& command,
      const std::vector<std::string>& args,
      const std::string& cwd = ""
    );
  };

} // namespace mistral::tools

#endif // MISTRAL_EXEC_TOOLS_H
