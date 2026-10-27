#include <doctest/doctest.h>
#include <dripcore/channel.h>
#include <dripcore/scheduler.h>

#include "console/console_task.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace
{
  // RAII PTY terminal that redirects stdin and stdout to a pseudo-terminal.
  //
  // Both stdin AND stdout are redirected so that isocline (which reads from
  // stdin and writes its prompt to stdout) operates entirely through the PTY.
  // The PTY master is used to inject keystrokes and observe prompt output.
  //
  struct pty_terminal
  {
    int master_fd = -1;
    int saved_stdin = -1;
    int saved_stdout = -1;

    pty_terminal()
    {
      master_fd = ::posix_openpt(O_RDWR | O_NOCTTY);
      REQUIRE(master_fd >= 0);
      REQUIRE(::grantpt(master_fd) == 0);
      REQUIRE(::unlockpt(master_fd) == 0);

      // Set master_fd to non-blocking mode
      int flags = ::fcntl(master_fd, F_GETFL, 0);
      REQUIRE(flags != -1);
      REQUIRE(::fcntl(master_fd, F_SETFL, flags | O_NONBLOCK) != -1);

      int slave_fd = ::open(::ptsname(master_fd), O_RDWR);
      REQUIRE(slave_fd >= 0);

      saved_stdin = ::dup(STDIN_FILENO);
      saved_stdout = ::dup(STDOUT_FILENO);
      ::dup2(slave_fd, STDIN_FILENO);
      ::dup2(slave_fd, STDOUT_FILENO);
      ::close(slave_fd);
    }

    ~pty_terminal()
    {
      close();
    }

    // Idempotent: safe to call explicitly and again from the destructor.
    void close()
    {
      if (saved_stdin >= 0)
      {
        ::dup2(saved_stdin, STDIN_FILENO);
        ::close(saved_stdin);
        saved_stdin = -1;
      }
      if (saved_stdout >= 0)
      {
        ::dup2(saved_stdout, STDOUT_FILENO);
        ::close(saved_stdout);
        saved_stdout = -1;
      }
      if (master_fd >= 0)
      {
        ::close(master_fd);
        master_fd = -1;
      }
    }

    // Wait until `text` appears in the master's output stream.  Returns true
    // if found within timeout_ms, false on timeout.  This is the
    // synchronization primitive: isocline writes its prompt to the PTY slave
    // (our stdout), which becomes readable on the master.  We wait for the
    // prompt text to confirm isocline is in its edit loop.
    bool wait_for_output(const std::string& text, int timeout_ms = 5000)
    {
      std::array<char, 4096> buf;
      std::string accumulated;

      auto start_time = std::chrono::steady_clock::now();
      while (true)
      {
        ssize_t n = ::read(master_fd, buf.data(), buf.size());
        if (n > 0)
        {
          accumulated.append(buf.data(), n);
          if (accumulated.find(text) != std::string::npos)
          {
            return true;
          }
        }
        else if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
          // Non-recoverable error (e.g., EBADF, EIO)
          return false;
        }

        // Check timeout
        auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
        if (elapsed >= timeout_ms)
        {
          return false;
        }

        // Small sleep to avoid busy-waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }

    // Continuously drain the master in the background.  This is essential:
    // isocline's tty_end_raw calls tcsetattr(TCSAFLUSH) which blocks until
    // all pending output on the slave has been read from the master.
    void drain_until(std::atomic<bool>& done, int timeout_ms = 10000)
    {
      struct pollfd pfd = {master_fd, POLLIN, 0};

      std::array<char, 4096> buf;

      auto start_time = std::chrono::steady_clock::now();
      while (!done.load())
      {
        auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();

        if (elapsed >= timeout_ms)
        {
          std::cerr << "drain_until timed out\n";
          break;
        }

        if (::poll(&pfd, 1, 50) > 0 && (pfd.revents & POLLIN))
        {
          ::read(master_fd, buf.data(), buf.size());
        }
      }
    }

    void send_keys(const std::string& data)
    {
      // Write in a loop to handle partial writes (EAGAIN)
      size_t offset = 0;
      while (offset < data.size())
      {
        ssize_t n = ::write(master_fd, data.c_str() + offset, data.size() - offset);
        if (n > 0)
        {
          offset += n;
        }
        else if (n == -1 && errno == EAGAIN)
        {
          // Non-blocking write would block; retry
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        else if (n == -1)
        {
          // Non-recoverable error
          break;
        }
      }
    }
  };
} // namespace

// The PTY driver runs in a plain thread (not a dripcore task) because the
// synchronization with isocline's terminal init (which runs in yet another
// thread, outside dripcore) requires poll()-with-timeout — the same approach
// used by expect/pexpect for PTY interaction.  Trying to synchronize purely
// through the dripcore event loop is fragile due to isocline's TCSAFLUSH
// on entering raw mode and macOS-specific OSC escape probes.
//
// The driver thread:
//   1. Sends the prompt signal via a pipe (wakes console_task)
//   2. Waits for the prompt to appear on the PTY master (poll + read)
//   3. Sends keystrokes to the PTY master
//   4. Reads the result from a pipe (console_task sends it after readline)
//
// Communication with console_task's channels is done through the dripcore
// event loop via the prompt/input pipe pair, not through direct channel access.

TEST_CASE("console_task: single line input")
{
  auto prompt_ch = std::make_shared<dripcore::channel<std::string>>(1);
  auto input_ch = std::make_shared<dripcore::channel<std::string>>(1);

  pty_terminal pty;

  // A dripcore task that triggers one prompt, collects the result, then exits.
  class collector_task : public dripcore::task
  {
  public:
    collector_task(
      std::shared_ptr<dripcore::channel<std::string>> prompt_ch,
      std::shared_ptr<dripcore::channel<std::string>> input_ch,
      std::string& result
    )
      : prompt_ch_(std::move(prompt_ch)), input_ch_(std::move(input_ch)), result_(result)
    {
    }

    dripcore::awaitable<> body() override
    {
      prompt_ch_->try_send("mistral");

      auto val = co_await input_ch_->recv(this);
      if (val)
        result_ = *val;

      prompt_ch_->close();
      co_return;
    }

  private:
    std::shared_ptr<dripcore::channel<std::string>> prompt_ch_;
    std::shared_ptr<dripcore::channel<std::string>> input_ch_;
    std::string& result_;
  };

  std::string result;

  dripcore::scheduler sched;
  sched.spawn<mistral::console::console_task>(prompt_ch, input_ch);
  sched.spawn<collector_task>(prompt_ch, input_ch, result);

  std::atomic<bool> done{false};

  // Driver thread: wait for prompt, send keys, then keep draining the PTY
  // master so that isocline's tty_end_raw (TCSAFLUSH) doesn't block.
  std::thread driver([&]() {
    REQUIRE(pty.wait_for_output("mistral"));
    pty.send_keys("Hello\r");
    pty.drain_until(done);
  });

  sched.run();
  done.store(true);
  driver.join();
  pty.close();

  CHECK(result == "Hello");
}
