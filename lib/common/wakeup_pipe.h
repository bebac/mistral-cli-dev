#ifndef MISTRAL_WAKEUP_PIPE_H
#define MISTRAL_WAKEUP_PIPE_H

#include <stdexcept>
#include <utility>
#include <unistd.h>

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
    }

    ~wakeup_pipe()
    {
      close_all();
    }

    wakeup_pipe(const wakeup_pipe&) = delete;
    wakeup_pipe& operator=(const wakeup_pipe&) = delete;

    wakeup_pipe(wakeup_pipe&& other) noexcept
      : fds_{other.fds_[0], other.fds_[1]}
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
      if (fds_[1] < 0) return false;
      ssize_t ret = ::write(fds_[1], &byte, 1);
      return ret == 1;
    }

    int read_fd() const { return fds_[0]; }
    int write_fd() const { return fds_[1]; }

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

    bool valid() const { return fds_[0] >= 0 && fds_[1] >= 0; }

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