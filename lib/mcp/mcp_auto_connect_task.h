#ifndef MISTRAL_MCP_AUTO_CONNECT_TASK_H
#define MISTRAL_MCP_AUTO_CONNECT_TASK_H

#include "agent/render_sink.h"

#include <dripcore/awaitable.h>
#include <dripcore/task.h>

#include <memory>

namespace mistral::mcp
{
  // Task that auto-connects MCP servers marked auto_connect=true.
  // Spawned at startup; runs once and exits.
  //
  // Progress and the outcome always go to stderr (console mode). When a
  // render sink is supplied they also go to the frontend's content area,
  // which is the only user-visible path in the TUI (its stderr is
  // redirected to a log file).
  //
  // The task pushes a ui_status snapshot once the connect finishes, so the
  // frontend never has to poll mcp_manager.
  class mcp_auto_connect_task : public dripcore::task
  {
  public:
    explicit mcp_auto_connect_task(std::shared_ptr<agent::render_sink> sink = nullptr);

    dripcore::awaitable<> body() override;

  private:
    std::shared_ptr<agent::render_sink> sink_;
  };
} // namespace mistral::mcp

#endif // MISTRAL_MCP_AUTO_CONNECT_TASK_H
