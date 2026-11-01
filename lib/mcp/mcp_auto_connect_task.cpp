#include "mcp/mcp_auto_connect_task.h"
#include "agent/agent_system.h"
#include "mcp/mcp_manager.h"

#include <format>
#include <iostream>
#include <utility>

namespace mistral::mcp
{
  mcp_auto_connect_task::mcp_auto_connect_task(std::shared_ptr<agent::render_sink> sink)
    : sink_(std::move(sink))
  {
  }

  dripcore::awaitable<> mcp_auto_connect_task::body()
  {
    auto& mgr = mcp_manager::instance();
    auto servers = mgr.auto_connect_servers();

    if (servers.empty())
    {
      co_return;
    }

    if (sink_)
    {
      sink_->output_status("Running MCP auto-connect...");
    }

    for (const auto& name : servers)
    {
      std::cerr << std::format("[mcp] auto-connecting '{}'...\n", name) << std::flush;
    }

    auto results = co_await mgr.connect_auto();

    // Server states and the tool registry changed: refresh the frontend
    // status (footer dot, completion candidates).
    agent::agent_system::instance().publish_ui_status();

    std::string errors;
    std::string warnings;

    for (const auto& r : results)
    {
      const std::string server = r["server"].get<std::string>();

      if (r.value("connected", false))
      {
        std::cerr << std::format(
                       "[mcp] connected '{}': {} tools registered\n", server, r["tools"].get<size_t>()
                     )
                  << std::flush;
      }
      else
      {
        const std::string error = r.value("error", std::string("unknown error"));
        std::cerr << std::format("[mcp] auto-connect failed for '{}': {}\n", server, error) << std::flush;
        std::cerr << "[mcp] run /mcp connect to retry\n" << std::flush;
        errors += std::format("  {}: {}\n", server, error);
      }

      // Non-fatal preflight/post_start_command problem (e.g. the
      // server connected but `xerj autoindex` exited non-zero).
      if (r.contains("warning"))
      {
        const std::string warning = r["warning"].get<std::string>();
        std::cerr << std::format("[mcp] warning '{}': {}\n", server, warning) << std::flush;
        warnings += std::format("  {}: {}\n", server, warning);
      }
    }

    if (sink_)
    {
      if (!errors.empty())
      {
        sink_->output_error("MCP auto-connect failed:\n" + errors);
      }
      if (!warnings.empty())
      {
        sink_->output_warning("MCP preflight warning:\n" + warnings);
      }
    }

    co_return;
  }
} // namespace mistral::mcp
