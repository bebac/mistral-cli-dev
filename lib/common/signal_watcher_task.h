#ifndef MISTRAL_SIGNAL_WATCHER_TASK_H
#define MISTRAL_SIGNAL_WATCHER_TASK_H

#include "agent/agent_system.h"

#include <dripcore/task.h>
#include <dripcore/task_handle.h>

#include <iostream>
#include <unistd.h>

namespace mistral::common
{
  // Monitors a self-pipe for SIGINT notifications and cancels the
  // current API request by closing the response channel registered
  // in agent_system.
  //
  // Protocol:
  //   Byte '1' -> SIGINT received, close current response channel
  //   Byte '0' -> shutdown requested, exit task
  //   EOF      -> pipe write end closed, exit task
  class signal_watcher_task : public dripcore::task
  {
  public:
    explicit signal_watcher_task(int pipe_read_fd)
      : pipe_read_fd_(pipe_read_fd)
    {
    }

    dripcore::task_handle execute()
    {
      while (true)
      {
        co_await readable(pipe_read_fd_);

        char buf[16];
        ssize_t n = ::read(pipe_read_fd_, buf, sizeof(buf));

        if (n <= 0) {
          // EOF or error -- shutdown.
          break;
        }

        for (ssize_t i = 0; i < n; i++)
        {
          if (buf[i] == '0') {
            // Shutdown signal.
            co_return;
          }
          if (buf[i] == '1') {
            // SIGINT -- cancel current request.
            auto ch = agent::agent_system::instance().get_current_response_ch();
            if (ch) {
              ch->close();
              std::cout << "\n^C" << std::endl;
            }
          }
        }
      }

      co_return;
    }

  private:
    int pipe_read_fd_;
  };
} // namespace mistral::common

#endif // MISTRAL_SIGNAL_WATCHER_TASK_H