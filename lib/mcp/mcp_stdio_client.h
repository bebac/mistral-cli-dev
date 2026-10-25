#ifndef MISTRAL_MCP_STDIO_CLIENT_H
#define MISTRAL_MCP_STDIO_CLIENT_H

#include "mcp/mcp_client_interface.h"

#include <dripcore/awaitable.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <sys/types.h>

namespace mistral::mcp
{
  using json = nlohmann::ordered_json;

  // MCP client for the stdio transport: spawns a child process (e.g.
  // `xerj mcp`) and speaks newline-delimited JSON-RPC 2.0 over its
  // stdin/stdout pipes. stderr is drained and discarded.
  //
  // Same MCP protocol surface as mcp_client (HTTP transport):
  // initialize, tools/list, tools/call.
  //
  // The child process is spawned with fork/exec and pipes, following
  // the pattern in lib/tools/exec_tools.cpp. I/O uses dripcore's
  // io_awaiter (POLLIN/POLLOUT) on the pipe file descriptors.
  class mcp_stdio_client : public mcp_client_interface
  {
  public:
    mcp_stdio_client(
      std::string command,
      std::vector<std::string> args = {},
      std::map<std::string, std::string> env = {},
      std::chrono::seconds request_timeout = std::chrono::seconds{30});

    ~mcp_stdio_client();

    mcp_stdio_client(const mcp_stdio_client&) = delete;
    mcp_stdio_client& operator=(const mcp_stdio_client&) = delete;
    mcp_stdio_client(mcp_stdio_client&&) = delete;
    mcp_stdio_client& operator=(mcp_stdio_client&&) = delete;

    // Spawns the child process with pipes. Returns false if the
    // command cannot be found or fork/exec fails.
    dripcore::awaitable<bool> connect();

    // MCP initialize handshake + notifications/initialized.
    // On success, server_info()/instructions() are populated.
    // Throws std::runtime_error on failure.
    dripcore::awaitable<bool> initialize() override;

    bool initialized() const override { return initialized_; }
    const json& server_info() const { return server_info_; }
    const std::string& instructions() const override { return instructions_; }

    // tools/list - returns the full result object (contains "tools").
    dripcore::awaitable<json> list_tools() override;

    // tools/call - returns the full result object (content[] and,
    // when the tool declares an outputSchema, structuredContent).
    dripcore::awaitable<json> call_tool(const std::string& name, const json& args) override;

    // Generic JSON-RPC request. Returns the "result" member; throws
    // std::runtime_error on transport failure or JSON-RPC error.
    dripcore::awaitable<json> request(const std::string& method, const json& params);

    // JSON-RPC notification (no response expected).
    dripcore::awaitable<bool> notify(const std::string& method);

    // Kill the child process and close pipes.
    void close();

    bool connected() const { return pid_ > 0; }

  private:
    // Write one JSON-RPC message as a newline-terminated line and
    // read response lines until the one with the matching id arrives.
    dripcore::awaitable<json> send_and_receive(const json& message, int expect_id);

    // Write a line (with trailing newline) to the child's stdin.
    // Returns false on write error / child exit.
    dripcore::awaitable<bool> write_line(const std::string& line);

    // Read one newline-terminated line from the child's stdout.
    // Returns nullopt on EOF / child exit / timeout.
    dripcore::awaitable<std::optional<std::string>> read_line();

    // Drain any available stderr output (non-blocking).
    void drain_stderr();

    std::string command_;
    std::vector<std::string> args_;
    std::map<std::string, std::string> env_;
    std::chrono::seconds timeout_;

    pid_t pid_ = -1;
    int stdin_fd_ = -1;    // parent writes -> child stdin
    int stdout_fd_ = -1;   // parent reads  <- child stdout
    int stderr_fd_ = -1;   // parent reads  <- child stderr (drained)

    std::string read_buffer_;  // partial line buffer for stdout
    bool initialized_ = false;
    json server_info_ = json::object();
    std::string instructions_;
    int next_id_ = 1;
  };

} // namespace mistral::mcp

#endif // MISTRAL_MCP_STDIO_CLIENT_H
