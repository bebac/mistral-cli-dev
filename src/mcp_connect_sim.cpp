// Simulates the /mcp connect flow: configures MCP servers (xerj with
// preflight + storybook HTTP), calls connect_all(), prints results.
// Use this to debug MCP connect issues without the TUI.
//
// Usage: ./mcp-connect-sim

#include "mcp/mcp_manager.h"
#include "agent/agent_config.h"

#include <dripcore/scheduler.h>
#include <dripcore/task.h>

#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>

#include <unistd.h>

namespace
{
  std::ofstream debug_log;

  void debug(const std::string& msg)
  {
    debug_log << msg << std::endl;
    debug_log.flush();
    std::cerr << "[sim] " << msg << std::endl;
  }

  class connect_task : public dripcore::task
  {
  public:
    dripcore::awaitable<> body() override
    {
      debug("=== mcp connect simulation ===");

      auto& mgr = mistral::mcp::mcp_manager::instance();

      // Configure xerj server (stdio + preflight) — same as the
      // workspace config.
      mistral::agent::mcp_server_config xerj_cfg;
      xerj_cfg.command = "xerj";
      xerj_cfg.args = {"mcp"};
      xerj_cfg.env = {{"XERJ_URL", "http://localhost:9200"}};
      xerj_cfg.check_url = "http://localhost:9200";
      xerj_cfg.start_command = {"xerj", "--insecure", "--data-dir", "~/xerj-data"};
      xerj_cfg.post_start_command = {"xerj", "autoindex", "."};

      // Configure storybook server (HTTP, no preflight).
      mistral::agent::mcp_server_config storybook_cfg;
      storybook_cfg.url = "http://127.0.0.1:6006/mcp";

      mgr.configure({{"xerj", xerj_cfg}, {"storybook", storybook_cfg}});
      debug("configured 2 servers");

      debug("calling connect_all()...");
      auto start = std::chrono::steady_clock::now();
      auto results = co_await mgr.connect_all();
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();

      debug("connect_all() returned after " + std::to_string(elapsed) + "s");
      debug("results: " + results.dump(2));

      // Print status
      auto status = mgr.status();
      debug("status: " + status.dump(2));

      co_return;
    }
  };
} // namespace

int main()
{
  debug_log.open("/tmp/mcp-connect-sim.log", std::ios::trunc);

  std::cerr << "=== MCP connect simulation ===" << std::endl;
  std::cerr << "Debug log: /tmp/mcp-connect-sim.log" << std::endl;

  // Ignore SIGPIPE (same as TUI/CLI — handle EPIPE explicitly).
  std::signal(SIGPIPE, SIG_IGN);

  dripcore::scheduler sched;
  sched.spawn<connect_task>();
  sched.run();

  // Close MCP clients (kill stdio child processes) before exit.
  mistral::mcp::mcp_manager::instance().shutdown();

  std::cerr << "=== scheduler done ===" << std::endl;
  debug("=== scheduler done ===");
  return 0;
}