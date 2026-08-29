#ifndef MISTRAL_SLASH_COMMAND_EXECUTOR_TASK_H
#define MISTRAL_SLASH_COMMAND_EXECUTOR_TASK_H

#include "console/slash_command.h"
#include "console/slash_command_registry.h"
#include "agent/agent_system.h"

#include <dripcore/task.h>
#include <dripcore/channel.h>
#include <dripcore/task_handle.h>
#include <dripcore/awaitable.h>

#include <memory>
#include <string>

namespace mistral::console
{
  /**
   * @brief Dedicated dripcore task for executing slash commands.
   */
  class slash_command_executor_task : public dripcore::task
  {
  public:
    slash_command_executor_task(
      std::shared_ptr<dripcore::channel<slash_command_request>> cmd_request_ch,
      std::shared_ptr<dripcore::channel<std::string>> slash_cmd_response_ch,
      std::shared_ptr<slash_command_registry> registry
    );

    dripcore::awaitable<> body() override;

  private:
    std::shared_ptr<dripcore::channel<slash_command_request>> cmd_request_ch_;
    std::shared_ptr<dripcore::channel<std::string>> slash_cmd_response_ch_;
    std::shared_ptr<slash_command_registry> registry_;
  };
} // namespace mistral::console

#endif // MISTRAL_SLASH_COMMAND_EXECUTOR_TASK_H