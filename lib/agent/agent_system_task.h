#ifndef MISTRAL_AGENT_SYSTEM_TASK_H
#define MISTRAL_AGENT_SYSTEM_TASK_H

#include "agent/agent_system.h"

#include <dripcore/task.h>
#include <dripcore/channel.h>
#include <dripcore/task_handle.h>
#include <dripcore/awaitable.h>

#include <memory>
#include <string>

namespace mistral::agent
{
  /**
   * @brief Task that receives AI responses and stores them in agent_system.
   *
   * This task runs in the background and listens on a channel for AI responses.
   * Each response is stored in the agent_system singleton, making it accessible
   * to slash commands and other components.
   */
  class agent_system_task : public dripcore::task
  {
  public:
    /**
     * @brief Construct a new agent_system_task.
     * @param agent_response_ch Channel to receive AI responses from.
     */
    explicit agent_system_task(
        std::shared_ptr<dripcore::channel<std::string>> agent_response_ch);

    dripcore::task_handle execute();

  private:
    std::shared_ptr<dripcore::channel<std::string>> agent_response_ch_;
  };
} // namespace mistral::agent

#endif // MISTRAL_AGENT_SYSTEM_TASK_H
