#include "console/slash_command_executor_task.h"
#include "console/slash_command_registry.h"

#include <arpa/inet.h>
#include <unistd.h>

#include <string>

namespace mistral::console
{
  slash_command_executor_task::slash_command_executor_task(
    std::shared_ptr<dripcore::channel<slash_command_request>> cmd_request_ch,
    int slash_resp_write_fd)
    :
    cmd_request_ch_(std::move(cmd_request_ch)),
    slash_resp_write_fd_(slash_resp_write_fd)
  {
  }

  dripcore::task_handle slash_command_executor_task::execute()
  {
    while (true)
    {
      auto request = co_await cmd_request_ch_->recv();

      if (!request) {
        break;
      }

      auto &registry = slash_command_registry::instance();
      const slash_command *const_cmd = registry.get_command(request->command_name);

      if (!const_cmd)
      {
        write_response("Unknown command: /" + request->command_name);
        continue;
      }

      if (const_cmd->context() != slash_command::execution_context::AGENT)
      {
        write_response("Command /" + request->command_name + " must be executed in IMMEDIATE context");
        continue;
      }

      std::string response;
      try
      {
        // const_cast is safe here because registry stores non-const commands
        slash_command *cmd = const_cast<slash_command *>(const_cmd);
        response = cmd->execute_agent(
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

      write_response(response);
    }
    co_return;
  }

  void slash_command_executor_task::write_response(const std::string &response)
  {
    if (slash_resp_write_fd_ == -1) {
      return;
    }

    uint32_t len = htonl(static_cast<uint32_t>(response.size()));
    ssize_t n = ::write(slash_resp_write_fd_, &len, sizeof(len));
    if (n != static_cast<ssize_t>(sizeof(len))) {
      return;
    }

    if (!response.empty())
    {
      ssize_t bytes_written = 0;
      const size_t total = response.size();
      while (bytes_written < static_cast<ssize_t>(total))
      {
        ssize_t chunk = ::write(
          slash_resp_write_fd_,
          response.data() + bytes_written,
          total - bytes_written
        );
        if (chunk <= 0)
          break;
        bytes_written += chunk;
      }
    }
  }
} // namespace mistral::console