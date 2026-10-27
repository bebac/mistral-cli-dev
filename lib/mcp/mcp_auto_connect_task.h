#ifndef MISTRAL_MCP_AUTO_CONNECT_TASK_H
#define MISTRAL_MCP_AUTO_CONNECT_TASK_H

#include <dripcore/awaitable.h>
#include <dripcore/task.h>

namespace mistral::mcp
{
  // Task that auto-connects MCP servers marked auto_connect=true.
  // Spawned at startup; runs once and exits. Prints status to stdout
  // (console mode) — the TUI polls mcp_manager status separately.
  class mcp_auto_connect_task : public dripcore::task
  {
  public:
    dripcore::awaitable<> body() override;
  };
} // namespace mistral::mcp

#endif // MISTRAL_MCP_AUTO_CONNECT_TASK_H
