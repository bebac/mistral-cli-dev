#include "console/slash_command_executor_task.h"
#include "console/slash_command_registry.h"

#include <arpa/inet.h>
#include <unistd.h>

#include <string>

namespace mistral::console
{
  slash_command_executor_task::slash_command_executor_task(
    std::shared_ptr<dripcore::channel<slash_command_request>> cmd_request_ch,
    std::shared_ptr<dripcore::channel<std::string>> slash_cmd_response_ch,
    std::shared_ptr<slash_command_registry> registry)
    :
    cmd_request_ch_(std::move(cmd_request_ch)),
    slash_cmd_response_ch_(std::move(slash_cmd_response_ch)),
    registry_(std::move(registry))
  {
  }

  dripcore::task_handle slash_command_executor_task::execute()
  {
    while (true)
    {
      auto request = co_await cmd_request_ch_->recv(this);

      if (!request) {
        break;
      }

      const slash_command *const_cmd = registry_->get_command(request->command_name);

      if (!const_cmd)
      {
        slash_cmd_response_ch_->try_send("Unknown command: /" + request->command_name);
        continue;
      }

      std::string response;
      try
      {
        // const_cast is safe here because registry stores non-const commands
        slash_command *cmd = const_cast<slash_command *>(const_cmd);
        response = co_await cmd->execute(
          *this,
          request->args,
          agent::agent_system::instance()
        );
      }
      catch (const std::exception &e)
      {
        response = std::string("Error executing /") + request->command_name + ": " + e.what();
      }
      catch (...)
      {
        response = "Unknown error executing /" + request->command_name;
      }

      slash_cmd_response_ch_->try_send(std::move(response));
    }
    co_return;
  }
} // namespace mistral::console