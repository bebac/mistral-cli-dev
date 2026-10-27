#include "mcp/mcp_stdio_client.h"

#include <dripcore/executor.h>
#include <dripcore/io_awaiter.h>
#include <dripcore/sleep_awaiter.h>

#include <cerrno>
#include <chrono>
#include <format>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace mistral::mcp
{
  mcp_stdio_client::mcp_stdio_client(
    std::string command,
    std::vector<std::string> args,
    std::map<std::string, std::string> env,
    std::chrono::seconds request_timeout
  )
    : command_(std::move(command)), args_(std::move(args)), env_(std::move(env)), timeout_(request_timeout)
  {
  }

  mcp_stdio_client::~mcp_stdio_client()
  {
    close();
  }

  dripcore::awaitable<bool> mcp_stdio_client::connect()
  {
    close();

    common::subprocess_options opts;
    opts.command = command_;
    opts.args = args_;
    opts.env_overrides = env_;
    opts.pipe_stdin = true; // parent writes JSON-RPC lines to the child's stdin

    if (process_.spawn(opts) != common::spawn_status::ok)
    {
      co_return false;
    }
    co_return true;
  }

  void mcp_stdio_client::close()
  {
    // Kills the child's process group, reaps without blocking and
    // closes the pipe fds (never blocks: see common::subprocess::kill).
    process_.kill();
    initialized_ = false;
    read_buffer_.clear();
  }

  void mcp_stdio_client::drain_stderr()
  {
    int fd = process_.stderr_fd();
    if (fd < 0)
      return;
    char buf[4096];
    while (true)
    {
      ssize_t n = ::read(fd, buf, sizeof(buf));
      if (n > 0)
        continue; // discard
      break;      // EAGAIN or EOF or error
    }
  }

  dripcore::awaitable<bool> mcp_stdio_client::write_line(const std::string& line)
  {
    int fd = process_.stdin_fd();
    if (fd < 0)
      co_return false;

    std::string data = line + "\n";
    size_t written = 0;
    auto deadline = std::chrono::steady_clock::now() + timeout_;

    while (written < data.size())
    {
      ssize_t n = ::write(fd, data.data() + written, data.size() - written);
      if (n > 0)
      {
        written += static_cast<size_t>(n);
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      {
        // Pipe full: poll with a short sleep (io_awaiter has no
        // timeout support; same pattern as the HTTP MCP client).
        if (std::chrono::steady_clock::now() >= deadline)
        {
          co_return false;
        }
        co_await dripcore::sleep_awaiter{dripcore::executor::current(), nullptr, std::chrono::milliseconds(5)};
        continue;
      }
      // Real error or child closed stdin (EPIPE).
      co_return false;
    }
    co_return true;
  }

  dripcore::awaitable<std::optional<std::string>> mcp_stdio_client::read_line()
  {
    int fd = process_.stdout_fd();
    if (fd < 0)
      co_return std::nullopt;

    auto deadline = std::chrono::steady_clock::now() + timeout_;

    while (true)
    {
      // Check if we already have a complete line in the buffer.
      auto pos = read_buffer_.find('\n');
      if (pos != std::string::npos)
      {
        std::string line = read_buffer_.substr(0, pos);
        read_buffer_.erase(0, pos + 1);
        // Strip trailing \r if present.
        if (!line.empty() && line.back() == '\r')
        {
          line.pop_back();
        }
        co_return line;
      }

      // Drain stderr while we wait (prevent child blocking on full
      // stderr pipe).
      drain_stderr();

      // Non-blocking read.
      char buf[4096];
      ssize_t n = ::read(fd, buf, sizeof(buf));
      if (n > 0)
      {
        read_buffer_.append(buf, static_cast<size_t>(n));
        continue;
      }
      if (n == 0)
      {
        // EOF: child closed stdout.
        co_return std::nullopt;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
        // No data: poll with a short sleep (io_awaiter has no
        // timeout support; same pattern as the HTTP MCP client).
        if (std::chrono::steady_clock::now() >= deadline)
        {
          co_return std::nullopt;
        }
        co_await dripcore::sleep_awaiter{dripcore::executor::current(), nullptr, std::chrono::milliseconds(5)};
        continue;
      }
      // Real error.
      co_return std::nullopt;
    }
  }

  dripcore::awaitable<json> mcp_stdio_client::send_and_receive(const json& message, int expect_id)
  {
    if (!process_.running())
    {
      if (!(co_await connect()))
      {
        throw std::runtime_error("cannot spawn mcp stdio server: " + command_);
      }
    }

    if (!(co_await write_line(message.dump())))
    {
      close();
      throw std::runtime_error("failed to write to mcp stdio server: " + command_);
    }

    // Read lines until we find the response with the matching id.
    // Notifications and server-initiated messages are skipped.
    while (true)
    {
      auto line = co_await read_line();
      if (!line)
      {
        close();
        throw std::runtime_error("mcp stdio server closed stdout: " + command_);
      }
      if (line->empty())
      {
        continue; // skip empty lines
      }

      json response;
      try
      {
        response = json::parse(*line);
      }
      catch (const std::exception&)
      {
        continue; // skip non-JSON lines (e.g. log output)
      }

      // Skip notifications (no id) and responses for other ids.
      if (!response.contains("id") || response["id"] != expect_id)
      {
        continue;
      }

      co_return response;
    }
  }

  dripcore::awaitable<json> mcp_stdio_client::request(const std::string& method, const json& params)
  {
    int id = next_id_++;
    json message = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
    if (!params.is_null())
    {
      message["params"] = params;
    }
    json response = co_await send_and_receive(message, id);

    if (response.contains("error"))
    {
      const auto& err = response["error"];
      throw std::runtime_error(
        std::format("mcp error {}: {}", err.value("code", 0), err.value("message", "unknown error"))
      );
    }
    co_return response.value("result", json::object());
  }

  dripcore::awaitable<bool> mcp_stdio_client::notify(const std::string& method)
  {
    json message = {{"jsonrpc", "2.0"}, {"method", method}};
    try
    {
      // Notifications get no response; we send and don't wait.
      // Use a dummy id of -1 and don't read a response.
      if (!process_.running())
      {
        if (!(co_await connect()))
          co_return false;
      }
      co_return co_await write_line(message.dump());
    }
    catch (const std::exception&)
    {
      co_return false;
    }
  }

  dripcore::awaitable<bool> mcp_stdio_client::initialize()
  {
    json params = {
      {"protocolVersion", "2025-06-18"},
      {"capabilities", json::object()},
      {"clientInfo", {{"name", "mistral-cli"}, {"version", "0.1.0"}}}
    };
    json result = co_await request("initialize", params);

    if (result.contains("serverInfo"))
    {
      server_info_ = result["serverInfo"];
    }
    instructions_ = result.value("instructions", std::string());

    co_await notify("notifications/initialized");
    initialized_ = true;
    co_return true;
  }

  dripcore::awaitable<json> mcp_stdio_client::list_tools()
  {
    co_return co_await request("tools/list", json());
  }

  dripcore::awaitable<json> mcp_stdio_client::call_tool(const std::string& name, const json& args)
  {
    json params = {{"name", name}, {"arguments", args}};
    co_return co_await request("tools/call", params);
  }

} // namespace mistral::mcp
