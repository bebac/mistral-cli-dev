#include "mcp/mcp_manager.h"

#include "agent/agent_system.h"
#include "common/executable_finder.h"
#include "mcp/mcp_client.h"
#include "mcp/mcp_stdio_client.h"
#include "mcp/mcp_tool.h"

#include <dripcore/executor.h>
#include <dripcore/io_awaiter.h>
#include <dripcore/sleep_awaiter.h>
#include <uri.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <stdexcept>

#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

// environ is not declared by <unistd.h> on all platforms (macOS).
extern char** environ;

namespace fs = std::filesystem;

namespace mistral::mcp
{
  namespace
  {
    // Expand a leading "~/" to the user's home directory. execve()
    // does not do shell expansion, so "~/xerj-data" would be treated
    // as a literal relative path.
    std::string expand_tilde(const std::string& path)
    {
      if (path.starts_with("~/") || path == "~")
      {
        const char* home = ::getenv("HOME");
        if (home)
        {
          return std::string(home) + path.substr(1);
        }
      }
      return path;
    }

    // Parse "http://host:port/path" into host, port, path.
    struct url_parts
    {
      std::string host;
      uint16_t port = 80;
      std::string path = "/";
    };

    std::optional<url_parts> parse_http_url(const std::string& url)
    {
      auto parsed = uri::parse(url);
      if (auto* err = std::get_if<uri::parse_error>(&parsed))
      {
        (void)err;
        return std::nullopt;
      }
      auto& u = std::get<uri::uri_view>(parsed);
      if (u.scheme() != "http")
        return std::nullopt;
      url_parts parts;
      parts.host = std::string(u.host());
      if (parts.host.empty())
        return std::nullopt;
      if (!u.port().empty())
      {
        parts.port = static_cast<uint16_t>(std::stoi(std::string(u.port())));
      }
      parts.path = u.path().empty() ? "/" : std::string(u.path());
      return parts;
    }
  } // namespace

  mcp_manager& mcp_manager::instance()
  {
    static mcp_manager mgr;
    return mgr;
  }

  void mcp_manager::configure(const std::map<std::string, agent::mcp_server_config>& servers)
  {
    servers_.clear();
    for (const auto& [name, cfg] : servers)
    {
      server_state st;
      st.config = cfg;
      servers_[name] = std::move(st);
    }
    instructions_.clear();
  }

  dripcore::awaitable<bool> mcp_manager::http_check(const std::string& url)
  {
    auto parts = parse_http_url(url);
    if (!parts)
      co_return false;

    // Overall deadline for the check (5 seconds). io_awaiter has no
    // timeout support, so we poll with sleep_awaiter instead — same
    // pattern as the HTTP MCP client and mcp_stdio_client.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    std::string port_str = std::to_string(parts->port);
    if (::getaddrinfo(parts->host.c_str(), port_str.c_str(), &hints, &result) != 0)
    {
      co_return false;
    }

    int fd = -1;
    for (addrinfo* rp = result; rp; rp = rp->ai_next)
    {
      fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
      if (fd < 0)
        continue;
      ::fcntl(fd, F_SETFL, O_NONBLOCK);
      int ret = ::connect(fd, rp->ai_addr, rp->ai_addrlen);
      if (ret == 0)
        break; // immediate connect
      if (errno == EINPROGRESS)
      {
        // Poll for connect completion. select() writable means the
        // connect finished (success OR failure) — must check SO_ERROR
        // AFTER select() to distinguish the two.
        bool connected = false;
        while (std::chrono::steady_clock::now() < deadline)
        {
          fd_set wfds;
          FD_ZERO(&wfds);
          FD_SET(fd, &wfds);
          timeval tv{0, 10000}; // 10ms
          int sel = ::select(fd + 1, nullptr, &wfds, nullptr, &tv);
          if (sel > 0)
          {
            // Socket is writable — connect finished. Check SO_ERROR
            // to see if it succeeded or failed.
            int err = 0;
            socklen_t len = sizeof(err);
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err == 0)
            {
              connected = true;
            }
            break;
          }
          if (sel < 0 && errno != EINTR)
          {
            break; // select error
          }
          // sel == 0: timeout, keep polling
          co_await dripcore::sleep_awaiter{dripcore::executor::current(), nullptr, std::chrono::milliseconds(10)};
        }
        if (connected)
          break;
      }
      ::close(fd);
      fd = -1;
    }
    ::freeaddrinfo(result);

    if (fd < 0)
      co_return false;

    // Send a minimal HTTP GET and check for any response.
    std::string request =
      std::format("GET {} HTTP/1.1\r\nHost: {}:{}\r\nConnection: close\r\n\r\n", parts->path, parts->host, parts->port);
    ssize_t sent = ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
    if (sent <= 0)
    {
      ::close(fd);
      co_return false;
    }

    // Poll for response data (non-blocking recv + sleep).
    char buf[256];
    ssize_t n = -1;
    while (std::chrono::steady_clock::now() < deadline)
    {
      n = ::recv(fd, buf, sizeof(buf) - 1, 0);
      if (n > 0)
        break; // got data
      if (n == 0)
        break; // EOF
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        break; // real error
      co_await dripcore::sleep_awaiter{dripcore::executor::current(), nullptr, std::chrono::milliseconds(10)};
    }
    ::close(fd);
    co_return n > 0;
  }

  pid_t mcp_manager::spawn_detached(const std::vector<std::string>& command)
  {
    if (command.empty())
      return -1;
    std::string exec_path = utils::find_executable(command[0]);
    if (exec_path.empty())
      return -1;

    // Build argv BEFORE fork() — strdup/malloc are not async-signal-safe
    // and the TUI is multi-threaded (scheduler + UI threads).
    // Expand "~/" in arguments (execve does not do shell expansion).
    std::vector<std::string> argv_strings;
    argv_strings.push_back(exec_path);
    for (size_t i = 1; i < command.size(); ++i)
    {
      argv_strings.push_back(expand_tilde(command[i]));
    }
    std::vector<char*> argv;
    for (auto& s : argv_strings)
    {
      argv.push_back(s.data());
    }
    argv.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid == -1)
      return -1;

    if (pid == 0)
    {
      // Child: detach into a new session, redirect stdio to /dev/null.
      // Only async-signal-safe calls from here until execve().
      ::setsid();
      int devnull = ::open("/dev/null", O_RDWR);
      if (devnull >= 0)
      {
        ::dup2(devnull, STDIN_FILENO);
        ::dup2(devnull, STDOUT_FILENO);
        ::dup2(devnull, STDERR_FILENO);
        if (devnull > 2)
          ::close(devnull);
      }

      // Close ALL other inherited file descriptors (kqueue fd,
      // wakeup pipe fds, MCP stdio pipe fds, etc.). Without this,
      // the detached child holds the parent's wakeup pipe write ends
      // open — when the parent closes its write ends during shutdown,
      // the pipes never see EOF and the dripcore event loop hangs.
      long max_fd = ::sysconf(_SC_OPEN_MAX);
      if (max_fd < 0)
        max_fd = 1024;
      for (long fd = 3; fd < max_fd; ++fd)
      {
        ::close(static_cast<int>(fd));
      }

      // execve (not execvp — execvp calls malloc internally).
      ::execve(exec_path.c_str(), argv.data(), environ);
      ::_exit(127);
    }

    // Parent: don't wait, the process is detached.
    return pid;
  }

  dripcore::awaitable<int> mcp_manager::run_command(const std::vector<std::string>& command, std::string* captured_output)
  {
    if (captured_output)
    {
      captured_output->clear();
    }
    if (command.empty())
      co_return -1;
    std::string exec_path = utils::find_executable(command[0]);
    if (exec_path.empty())
      co_return -1;

    int out_pipe[2];
    int err_pipe[2];
    if (::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0)
      co_return -1;

    // Build argv BEFORE fork() — strdup/malloc are not async-signal-safe
    // and the TUI is multi-threaded (scheduler + UI threads).
    // Expand "~/" in arguments (execve does not do shell expansion).
    std::vector<std::string> argv_strings;
    argv_strings.push_back(exec_path);
    for (size_t i = 1; i < command.size(); ++i)
    {
      argv_strings.push_back(expand_tilde(command[i]));
    }
    std::vector<char*> argv;
    for (auto& s : argv_strings)
    {
      argv.push_back(s.data());
    }
    argv.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid == -1)
      co_return -1;

    if (pid == 0)
    {
      // Child. Only async-signal-safe calls from here until execve().
      ::setpgid(0, 0);
      ::dup2(out_pipe[1], STDOUT_FILENO);
      ::dup2(err_pipe[1], STDERR_FILENO);
      ::close(out_pipe[0]);
      ::close(out_pipe[1]);
      ::close(err_pipe[0]);
      ::close(err_pipe[1]);

      // Close ALL other inherited file descriptors (kqueue fd,
      // wakeup pipe fds, MCP stdio pipe fds, etc.). Without this,
      // the child holds the parent's wakeup pipe write ends open —
      // when the parent closes its write ends during shutdown, the
      // pipes never see EOF and the dripcore event loop hangs.
      long max_fd = ::sysconf(_SC_OPEN_MAX);
      if (max_fd < 0)
        max_fd = 1024;
      for (long fd = 3; fd < max_fd; ++fd)
      {
        ::close(static_cast<int>(fd));
      }

      // execve (not execvp — execvp calls malloc internally).
      ::execve(exec_path.c_str(), argv.data(), environ);
      ::_exit(127);
    }

    // Parent: close write ends, drain output, wait.
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    ::fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
    ::fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);

    // Capture output (stdout + stderr interleaved), keeping only the
    // tail — early output (progress bars, scan logs) is noise, the
    // error message is at the end.
    constexpr size_t k_max_captured_output = 4096;
    std::string output;
    auto append_output = [&output](const char* data, ssize_t n) {
      output.append(data, static_cast<size_t>(n));
      if (output.size() > k_max_captured_output)
      {
        output.erase(0, output.size() - k_max_captured_output);
      }
    };

    // Drain pipes while waiting for the child (60s timeout — e.g.
    // `xerj autoindex` on a large repo can take a while, but it
    // should never hang forever).
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    char buf[4096];
    ssize_t n;
    while (true)
    {
      // Non-blocking drain.
      while ((n = ::read(out_pipe[0], buf, sizeof(buf))) > 0)
      {
        append_output(buf, n);
      }
      while ((n = ::read(err_pipe[0], buf, sizeof(buf))) > 0)
      {
        append_output(buf, n);
      }

      int status = 0;
      pid_t w = ::waitpid(pid, &status, WNOHANG);
      if (w == pid)
      {
        // Final drain: the child has exited, so its pipe write ends
        // are closed — read() returns 0 (EOF) once the buffered data
        // is consumed. Bounded in case a grandchild still holds the
        // write end (then there is no EOF).
        for (int i = 0; i < 100; ++i)
        {
          bool any = false;
          while ((n = ::read(out_pipe[0], buf, sizeof(buf))) > 0)
          {
            append_output(buf, n);
            any = true;
          }
          while ((n = ::read(err_pipe[0], buf, sizeof(buf))) > 0)
          {
            append_output(buf, n);
            any = true;
          }
          if (!any)
            break;
        }
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);
        if (captured_output)
        {
          *captured_output = std::move(output);
        }
        if (WIFEXITED(status))
          co_return WEXITSTATUS(status);
        co_return -1;
      }
      if (w < 0)
      {
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);
        if (captured_output)
        {
          *captured_output = std::move(output);
        }
        co_return -1;
      }
      // Timeout: kill the child and bail out.
      if (std::chrono::steady_clock::now() >= deadline)
      {
        ::kill(-pid, SIGKILL);
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);
        if (captured_output)
        {
          *captured_output = std::move(output);
        }
        co_return -1;
      }
      // Yield to the scheduler.
      co_await dripcore::sleep_awaiter{dripcore::executor::current(), nullptr, std::chrono::milliseconds(10)};
    }
  }

  dripcore::awaitable<void> mcp_manager::run_post_start(const agent::mcp_server_config& cfg, std::string* warning)
  {
    if (cfg.post_start_command.empty())
      co_return;

    // A non-zero exit code is NOT fatal — the server is already up,
    // and tools like `xerj autoindex` use non-zero codes both for
    // hard failures (exit 1) and warnings (exit 3 = some files
    // skipped). Capture the output tail so the failure is visible
    // instead of silent.
    std::string output;
    int exit_code = co_await run_command(cfg.post_start_command, &output);
    if (exit_code == 0 || warning == nullptr || !warning->empty())
      co_return;

    std::string cmd;
    for (size_t i = 0; i < cfg.post_start_command.size(); ++i)
    {
      if (i > 0)
        cmd += " ";
      cmd += cfg.post_start_command[i];
    }
    *warning = std::format("post_start_command '{}' exited with {}", cmd, exit_code);
    if (!output.empty())
    {
      *warning += std::format(": {}", output);
    }
  }

  dripcore::awaitable<std::string> mcp_manager::run_preflight(const agent::mcp_server_config& cfg, std::string* warning)
  {
    if (cfg.check_url.empty())
      co_return "";

    // 1. Check if the URL is already reachable.
    if (co_await http_check(cfg.check_url))
    {
      // Already running. Run post_start_command if present
      // (e.g. autoindex to refresh the index).
      co_await run_post_start(cfg, warning);
      co_return "";
    }

    // 2. Not running: spawn start_command as a detached process.
    if (cfg.start_command.empty())
    {
      co_return std::format(
        "preflight check_url '{}' is not reachable and no start_command "
        "is configured",
        cfg.check_url
      );
    }

    pid_t pid = spawn_detached(cfg.start_command);
    if (pid < 0)
    {
      co_return "preflight start_command failed to spawn";
    }

    // 3. Wait for check_url to become reachable (up to 15 seconds).
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline)
    {
      if (co_await http_check(cfg.check_url))
      {
        // 4. Run post_start_command (e.g. autoindex). Non-fatal
        //    (see run_post_start).
        co_await run_post_start(cfg, warning);
        co_return "";
      }
      co_await dripcore::sleep_awaiter{dripcore::executor::current(), nullptr, std::chrono::milliseconds(500)};
    }

    co_return std::format("preflight timed out waiting for '{}' to become reachable", cfg.check_url);
  }

  dripcore::awaitable<json> mcp_manager::connect(const std::string& name)
  {
    auto it = servers_.find(name);
    if (it == servers_.end())
    {
      co_return json{{"error", "unknown mcp server '" + name + "'"}};
    }

    server_state& st = it->second;
    try
    {
      // Run preflight (if configured).
      st.warning.clear();
      if (!st.config.check_url.empty())
      {
        std::string preflight_warning;
        std::string preflight_error = co_await run_preflight(st.config, &preflight_warning);
        st.warning = preflight_warning;
        if (!preflight_error.empty())
        {
          throw std::runtime_error(preflight_error);
        }
      }

      // Create the client (HTTP or stdio transport).
      if (st.config.is_stdio())
      {
        st.client = std::make_shared<mcp_stdio_client>(st.config.command, st.config.args, st.config.env);
      }
      else
      {
        st.client = std::make_shared<mcp_client>(st.config.url);
      }

      if (!(co_await st.client->initialize()))
      {
        throw std::runtime_error("initialize failed");
      }

      json tools_result = co_await st.client->list_tools();
      st.connected = true;
      st.error.clear();
      st.instructions = st.client->instructions();
      st.tool_names.clear();

      auto& registry = agent::agent_system::instance().global_tools();
      size_t registered = 0;
      if (tools_result.contains("tools") && tools_result["tools"].is_array())
      {
        for (const auto& t : tools_result["tools"])
        {
          if (!t.contains("name"))
            continue;
          std::string tool_name = t["name"].get<std::string>();
          registry.add_tool(
            tool_name,
            std::make_shared<mcp_tool>(
              st.client,
              tool_name,
              t.value("description", std::string()),
              t.contains("inputSchema") ? t["inputSchema"] : json::object()
            )
          );
          st.tool_names.push_back(tool_name);
          ++registered;
        }
      }

      rebuild_instructions();
      ++registry_version_;

      json result = json{
        {"server", name}, {"connected", true}, {"tools", registered}, {"instructions_chars", st.instructions.size()}
      };
      if (!st.warning.empty())
      {
        result["warning"] = st.warning;
      }
      co_return result;
    }
    catch (const std::exception& e)
    {
      st.connected = false;
      st.error = e.what();
      st.client.reset();
      json result = json{{"server", name}, {"connected", false}, {"error", st.error}};
      if (!st.warning.empty())
      {
        result["warning"] = st.warning;
      }
      co_return result;
    }
  }

  dripcore::awaitable<json> mcp_manager::connect_all()
  {
    json results = json::array();
    for (const auto& [name, st] : servers_)
    {
      results.push_back(co_await connect(name));
    }
    co_return results;
  }

  dripcore::awaitable<json> mcp_manager::connect_auto()
  {
    json results = json::array();
    for (const auto& [name, st] : servers_)
    {
      if (st.config.auto_connect)
      {
        results.push_back(co_await connect(name));
      }
    }
    auto_connect_finished_ = true;
    co_return results;
  }

  json mcp_manager::auto_connect_report() const
  {
    // Per-server outcome of the last connect_auto() run, for the TUI
    // content area: failures and preflight/post_start warnings.
    json out = json::array();
    for (const auto& [name, st] : servers_)
    {
      if (!st.config.auto_connect)
        continue;
      json r = {{"server", name}, {"connected", st.connected}};
      if (!st.error.empty())
      {
        r["error"] = st.error;
      }
      if (!st.warning.empty())
      {
        r["warning"] = st.warning;
      }
      out.push_back(std::move(r));
    }
    return out;
  }

  std::vector<std::string> mcp_manager::auto_connect_servers() const
  {
    std::vector<std::string> names;
    for (const auto& [name, st] : servers_)
    {
      if (st.config.auto_connect)
      {
        names.push_back(name);
      }
    }
    return names;
  }

  bool mcp_manager::auto_connect_all_connected() const
  {
    bool any = false;
    for (const auto& [name, st] : servers_)
    {
      if (st.config.auto_connect)
      {
        any = true;
        if (!st.connected)
          return false;
      }
    }
    return any;
  }

  void mcp_manager::shutdown()
  {
    // Close all clients. For stdio clients this kills the child
    // process (non-blocking — see mcp_stdio_client::close).
    for (auto& [name, st] : servers_)
    {
      if (st.client)
      {
        // mcp_client_interface has no close() — the stdio client's
        // destructor calls close(). Reset the shared_ptr to trigger
        // destruction now (during shutdown) rather than from the
        // static destructor during process exit.
        st.client.reset();
      }
      st.connected = false;
    }
  }

  json mcp_manager::status() const
  {
    json out = json::array();
    for (const auto& [name, st] : servers_)
    {
      json s = {{"name", name}, {"connected", st.connected}};
      if (st.config.is_stdio())
      {
        s["transport"] = "stdio";
        s["command"] = st.config.command;
      }
      else
      {
        s["transport"] = "http";
        s["url"] = st.config.url;
      }
      if (!st.error.empty())
      {
        s["error"] = st.error;
      }
      if (!st.warning.empty())
      {
        s["warning"] = st.warning;
      }
      if (st.connected)
      {
        s["tools"] = st.tool_names;
      }
      out.push_back(std::move(s));
    }
    return out;
  }

  std::vector<std::string> mcp_manager::tool_names(const std::string& server) const
  {
    std::vector<std::string> names;
    if (server.empty() || server == "*")
    {
      for (const auto& [name, st] : servers_)
      {
        if (st.connected)
        {
          names.insert(names.end(), st.tool_names.begin(), st.tool_names.end());
        }
      }
    }
    else
    {
      auto it = servers_.find(server);
      if (it != servers_.end() && it->second.connected)
      {
        names = it->second.tool_names;
      }
    }
    return names;
  }

  void mcp_manager::rebuild_instructions()
  {
    instructions_.clear();
    for (const auto& [name, st] : servers_)
    {
      if (st.connected && !st.instructions.empty())
      {
        if (!instructions_.empty())
        {
          instructions_ += "\n\n";
        }
        instructions_ += "# MCP server: " + name + "\n\n" + st.instructions;
      }
    }
  }

} // namespace mistral::mcp
