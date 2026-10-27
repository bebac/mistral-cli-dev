#include "mcp/mcp_auto_connect_task.h"
#include "mcp/mcp_manager.h"

#include <format>
#include <iostream>

namespace mistral::mcp
{
  dripcore::awaitable<> mcp_auto_connect_task::body()
  {
    auto& mgr = mcp_manager::instance();
    auto servers = mgr.auto_connect_servers();

    if (servers.empty())
    {
      co_return;
    }

    for (const auto& name : servers)
    {
      std::cerr << std::format("[mcp] auto-connecting '{}'...\n", name) << std::flush;
    }

    auto results = co_await mgr.connect_auto();

    for (const auto& r : results)
    {
      if (r.value("connected", false))
      {
        std::cerr << std::format(
                       "[mcp] connected '{}': {} tools registered\n",
                       r["server"].get<std::string>(),
                       r["tools"].get<size_t>()
                     )
                  << std::flush;
      }
      else
      {
        std::cerr << std::format(
                       "[mcp] auto-connect failed for '{}': {}\n",
                       r["server"].get<std::string>(),
                       r.value("error", std::string("unknown error"))
                     )
                  << std::flush;
        std::cerr << "[mcp] run /mcp connect to retry\n" << std::flush;
      }

      // Non-fatal preflight/post_start_command problem (e.g. the
      // server connected but `xerj autoindex` exited non-zero).
      if (r.contains("warning"))
      {
        std::cerr << std::format(
                       "[mcp] warning '{}': {}\n",
                       r["server"].get<std::string>(),
                       r["warning"].get<std::string>()
                     )
                  << std::flush;
      }
    }

    co_return;
  }
} // namespace mistral::mcp
