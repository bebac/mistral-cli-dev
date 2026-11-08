#include "tools/process_tools.h"

#include "common/process_manager.h"

#include <dripcore/executor.h>
#include <dripcore/io_awaiter.h>
#include <dripcore/sleep_awaiter.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <map>
#include <regex>
#include <unistd.h>

namespace mistral::tools
{
  namespace
  {
    constexpr int k_default_max_read_bytes = 64 * 1024;
    constexpr int k_default_wait_timeout_seconds = 60;

    json error_result(std::string message)
    {
      return {{"error", std::move(message)}};
    }
  } // namespace

  dripcore::awaitable<json> process_tools::process_start(
    std::string command,
    std::vector<std::string> args,
    std::string cwd,
    std::vector<std::string> env,
    std::string name,
    std::string wait_for,
    int wait_timeout_seconds
  )
  {
    // env entries are "KEY=VALUE"; reject anything without '='.
    std::map<std::string, std::string> env_map;
    for (const auto& entry : env)
    {
      auto pos = entry.find('=');
      if (pos == std::string::npos || pos == 0)
      {
        co_return error_result(std::format("invalid env entry '{}': expected KEY=VALUE", entry));
      }
      env_map[entry.substr(0, pos)] = entry.substr(pos + 1);
    }

    auto started = mgr_.start(command, args, cwd, env_map, name);
    if (!started.ok())
    {
      co_return error_result(started.error);
    }
    const std::string id = started.id;

    json result{
      {"id", id},
      {"pid", started.pid},
      {"command", command},
      {"args", args},
    };

    // wait_for: poll the session's output until the pattern shows up,
    // the process exits, or the deadline passes.
    bool ready = true;
    if (!wait_for.empty())
    {
      std::regex pattern;
      try
      {
        pattern = std::regex(wait_for);
      }
      catch (const std::regex_error& e)
      {
        co_return json{{"error", std::format("invalid wait_for regex: {}", e.what())}, {"id", id}};
      }
      int timeout = wait_timeout_seconds > 0 ? wait_timeout_seconds : k_default_wait_timeout_seconds;
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
      ready = false;
      while (true)
      {
        if (mgr_.output_matches(id, pattern))
        {
          ready = true;
          break;
        }
        int exit_code = 0;
        if (mgr_.exited(id, exit_code))
        {
          break; // process ended before the pattern appeared
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
          break;
        }
        co_await dripcore::sleep_awaiter{
          dripcore::executor::current(), nullptr, std::chrono::milliseconds(50)
        };
      }
    }

    if (auto* s = mgr_.find(id))
    {
      result["running"] = s->sub.running();
    }
    result["ready"] = ready;
    co_return result;
  }

  dripcore::awaitable<json> process_tools::process_write(std::string id, std::string input)
  {
    int fd = mgr_.stdin_fd(id);
    if (fd < 0)
    {
      co_return error_result(std::format("unknown session (or no stdin pipe): {}", id));
    }
    if (auto* s = mgr_.find(id); s && s->exited)
    {
      int exit_code = 0;
      mgr_.exited(id, exit_code);
      co_return json{{"error", "process has exited"}, {"id", id}, {"exit_code", exit_code}};
    }

    // The fd is non-blocking: write what fits now, wait for POLLOUT
    // on EAGAIN. SIGPIPE is ignored by every CLI entry point, so a
    // dead child surfaces as EPIPE here instead of killing the CLI.
    size_t written = 0;
    while (written < input.size())
    {
      ssize_t n = ::write(fd, input.data() + written, input.size() - written);
      if (n > 0)
      {
        written += static_cast<size_t>(n);
        continue;
      }
      if (n < 0 && errno == EINTR)
      {
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      {
        co_await dripcore::io_awaiter{fd, POLLOUT, dripcore::executor::current(), nullptr};
        continue;
      }
      co_return json{
        {"error", std::format("write to stdin failed: {}", strerror(errno))},
        {"id", id},
        {"written", written},
      };
    }
    co_return json{{"id", id}, {"written", written}};
  }

  json process_tools::process_read(std::string id, int64_t since, int max_bytes)
  {
    if (max_bytes <= 0)
    {
      max_bytes = k_default_max_read_bytes;
    }
    auto r = mgr_.read(id, since < 0 ? 0 : static_cast<uint64_t>(since), static_cast<size_t>(max_bytes));
    if (!r.found)
    {
      return error_result(std::format("unknown session: {}", id));
    }
    json result{
      {"id", id},
      {"output", r.output},
      {"next_offset", r.next_offset},
      {"total_bytes", r.total_bytes},
      {"truncated", r.truncated},
      {"running", r.running},
      {"exited", r.exited},
    };
    if (r.exited)
    {
      result["exit_code"] = r.exit_code;
    }
    return result;
  }

  json process_tools::process_list()
  {
    json sessions = json::array();
    for (const auto& info : mgr_.list())
    {
      json entry{
        {"id", info.id},
        {"name", info.name},
        {"command", info.command},
        {"args", info.args},
        {"pid", info.pid},
        {"running", info.running},
        {"exited", info.exited},
        {"total_output_bytes", info.total_bytes},
        {"uptime_seconds", info.uptime_seconds},
      };
      if (info.exited)
      {
        entry["exit_code"] = info.exit_code;
      }
      sessions.push_back(std::move(entry));
    }
    // Capture the count before moving the array into the result.
    json result;
    result["count"] = sessions.size();
    result["sessions"] = std::move(sessions);
    return result;
  }

  json process_tools::process_kill(std::string id)
  {
    if (!mgr_.kill(id))
    {
      return error_result(std::format("unknown session: {}", id));
    }
    json result{{"id", id}, {"killed", true}};
    int exit_code = 0;
    if (mgr_.exited(id, exit_code))
    {
      result["exit_code"] = exit_code;
    }
    return result;
  }

  std::optional<exec_request> process_tools::parse_request(const json& args)
  {
    if (!args.is_object() || !args.contains("command") || !args["command"].is_string())
    {
      return std::nullopt;
    }

    exec_request request;
    request.command = args["command"].get<std::string>();

    if (args.contains("args"))
    {
      if (!args["args"].is_array())
      {
        return std::nullopt;
      }
      for (const auto& arg : args["args"])
      {
        if (!arg.is_string())
        {
          return std::nullopt;
        }
        request.args.push_back(arg.get<std::string>());
      }
    }

    if (args.contains("cwd"))
    {
      if (!args["cwd"].is_string())
      {
        return std::nullopt;
      }
      request.cwd = args["cwd"].get<std::string>();
    }

    return request;
  }
} // namespace mistral::tools
