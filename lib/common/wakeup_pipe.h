#ifndef MISTRAL_WAKEUP_PIPE_H
#define MISTRAL_WAKEUP_PIPE_H

#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>
#include <utility>

namespace mistral::common
{
  class wakeup_pipe
  {
  public:
    wakeup_pipe()
    {
      if (::pipe(fds_) != 0)
      {
        throw std::runtime_error("failed to create wakeup pipe");
      }
      // Set FD_CLOEXEC so child processes (e.g. MCP stdio servers
      // spawned via fork/exec) do NOT inherit these fds. Without
      // this, the child holds the write end open, and when the
      // parent closes its write end during shutdown, the pipe never
      // sees EOF — the dripcore tasks block forever on readable()
      // and sched.run() never returns (CTRL-C hang).
      ::fcntl(fds_[0], F_SETFD, FD_CLOEXEC);
      ::fcntl(fds_[1], F_SETFD, FD_CLOEXEC);
    }

    ~wakeup_pipe()
    {
      close_all();
    }

    wakeup_pipe(const wakeup_pipe&) = delete;
    wakeup_pipe& operator=(const wakeup_pipe&) = delete;

    wakeup_pipe(wakeup_pipe&& other) noexcept : fds_{other.fds_[0], other.fds_[1]}
    {
      other.fds_[0] = -1;
      other.fds_[1] = -1;
    }

    wakeup_pipe& operator=(wakeup_pipe&& other) noexcept
    {
      if (this != &other)
      {
        close_all();
        fds_[0] = other.fds_[0];
        fds_[1] = other.fds_[1];
        other.fds_[0] = -1;
        other.fds_[1] = -1;
      }
      return *this;
    }

    bool notify(char byte = '1')
    {
      if (fds_[1] < 0)
        return false;
      ssize_t ret = ::write(fds_[1], &byte, 1);
      return ret == 1;
    }

    int read_fd() const
    {
      return fds_[0];
    }

    int write_fd() const
    {
      return fds_[1];
    }

    void close_write()
    {
      if (fds_[1] >= 0)
      {
        ::close(fds_[1]);
        fds_[1] = -1;
      }
    }

    void close_read()
    {
      if (fds_[0] >= 0)
      {
        ::close(fds_[0]);
        fds_[0] = -1;
      }
    }

    std::pair<int, int> release()
    {
      int r = fds_[0];
      int w = fds_[1];
      fds_[0] = -1;
      fds_[1] = -1;
      return {r, w};
    }

    bool valid() const
    {
      return fds_[0] >= 0 && fds_[1] >= 0;
    }

  private:
    void close_all()
    {
      close_read();
      close_write();
    }

    int fds_[2] = {-1, -1};
  };

} // namespace mistral::common

#endif // MISTRAL_WAKEUP_PIPE_H
