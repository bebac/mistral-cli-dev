#include "agent/agent_system_stats_task.h"
#include "agent/agent_system.h"

namespace mistral::agent
{
  agent_system_stats_task::agent_system_stats_task(std::shared_ptr<dripcore::channel<stat_message>> stats_ch)
    : stats_ch_(std::move(stats_ch))
  {
  }

  dripcore::awaitable<> agent_system_stats_task::body()
  {
    while (true)
    {
      auto msg = co_await stats_ch_->recv(this);
      if (!msg)
      {
        break;
      }

      switch (msg->id)
      {
      case stat_id::api_usage:
      {
        auto& data = msg->data;
        agent_system::instance().update_usage_stats(data);
      }
      break;
      }
    }
    co_return;
  }
} // namespace mistral::agent
