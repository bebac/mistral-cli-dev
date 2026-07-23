#ifndef MISTRAL_SLASH_COMMAND_EXECUTOR_TASK_H
#define MISTRAL_SLASH_COMMAND_EXECUTOR_TASK_H

#include "console/slash_command.h"
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
   * @brief Dedicated dripcore task for executing slash commands that require agent context.
   */
  class slash_command_executor_task : public dripcore::task
  {
  public:
    slash_command_executor_task(
      std::shared_ptr<dripcore::channel<slash_command_request>> cmd_request_ch,
      int slash_resp_write_fd
    );

    dripcore::task_handle execute();

  private:
    std::shared_ptr<dripcore::channel<slash_command_request>> cmd_request_ch_;
    int slash_resp_write_fd_ = -1;

    void write_response(const std::string &response);
  };
} // namespace mistral::console

#endif // MISTRAL_SLASH_COMMAND_EXECUTOR_TASK_H