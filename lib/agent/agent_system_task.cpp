#include "agent/agent_system_task.h"

namespace mistral::agent
{
  agent_system_task::agent_system_task(
      std::shared_ptr<dripcore::channel<std::string>> agent_response_ch)
      : agent_response_ch_(std::move(agent_response_ch))
  {
  }

  dripcore::task_handle agent_system_task::execute()
  {
    while (true)
    {
      auto response = co_await agent_response_ch_->recv(this);
      if (!response)
      {
        // Channel closed, task is done
        break;
      }

      // Store the response in agent_system
      agent_system::instance().set_last_response(*response);
    }
    co_return;
  }
} // namespace mistral::agent
