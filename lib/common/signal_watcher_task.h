#ifndef MISTRAL_SIGNAL_WATCHER_TASK_H
#define MISTRAL_SIGNAL_WATCHER_TASK_H

#include "agent/agent_system.h"

#include <dripcore/task.h>
#include <dripcore/task_handle.h>

#include <unistd.h>

namespace mistral::common
{
  // Monitors a self-pipe for SIGINT notifications and cancels whatever
  // the agent_system has registered as the current cancellable operation
  // (an API response stream, or a running exec).
  //
  // Protocol:
  //   Byte '1' -> SIGINT received, cancel the current operation
  //   Byte '0' -> shutdown requested, exit task
  //   EOF      -> pipe write end closed, exit task
  class signal_watcher_task : public dripcore::task
  {
  public:
    explicit signal_watcher_task(int pipe_read_fd) : pipe_read_fd_(pipe_read_fd)
    {
    }

    dripcore::awaitable<> body() override
    {
      while (true)
      {
        co_await readable(pipe_read_fd_);

        char buf[16];
        ssize_t n = ::read(pipe_read_fd_, buf, sizeof(buf));

        if (n <= 0)
        {
          // EOF or error -- shutdown.
          agent::agent_system::instance().stop_transport();
          break;
        }

        for (ssize_t i = 0; i < n; i++)
        {
          if (buf[i] == '0')
          {
            // Shutdown signal.
            agent::agent_system::instance().stop_transport();
            co_return;
          }
          if (buf[i] == '1')
          {
            // SIGINT -- cancel whatever is currently cancellable.
            agent::agent_system::instance().cancel_current();
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
