#ifndef PTY_TEST_HARNESS_H
#define PTY_TEST_HARNESS_H

#include <doctest/doctest.h>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <termios.h>

#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// pty_test: reusable PTY test harness for cpp-tui applications.
//
// pty_terminal redirects stdin/stdout to a pseudo-terminal so tests can
// inject keystrokes and observe rendered output. drain_wait prevents
// deadlock during shutdown (Terminal::~Terminal calls tcsetattr(TCSAFLUSH)
// which blocks until the master is drained).
// ---------------------------------------------------------------------------
namespace pty_test {

struct pty_terminal {
  int master_fd    = -1;
  int saved_stdin  = -1;
  int saved_stdout = -1;
  int saved_stderr = -1;

  pty_terminal() {
    std::cout.flush();
    std::cerr.flush();

    master_fd = ::posix_openpt(O_RDWR | O_NOCTTY);
    REQUIRE(master_fd >= 0);
    REQUIRE(::grantpt(master_fd) == 0);
    REQUIRE(::unlockpt(master_fd) == 0);

    int flags = ::fcntl(master_fd, F_GETFL, 0);
    REQUIRE(flags != -1);
    REQUIRE(::fcntl(master_fd, F_SETFL, flags | O_NONBLOCK) != -1);

    int slave_fd = ::open(::ptsname(master_fd), O_RDWR);
    REQUIRE(slave_fd >= 0);

    struct winsize ws;
    ws.ws_col   = 80;
    ws.ws_row   = 24;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
    ::ioctl(slave_fd, TIOCSWINSZ, &ws);

    saved_stdin   = ::dup(STDIN_FILENO);
    saved_stdout  = ::dup(STDOUT_FILENO);
    saved_stderr  = ::dup(STDERR_FILENO);
    ::dup2(slave_fd, STDIN_FILENO);
    ::dup2(slave_fd, STDOUT_FILENO);
    ::dup2(slave_fd, STDERR_FILENO);
    ::close(slave_fd);
  }

  ~pty_terminal() {
    restore();
    if (master_fd >= 0) ::close(master_fd);
  }

  void restore() {
    if (saved_stdin >= 0) {
      ::dup2(saved_stdin, STDIN_FILENO);
      ::close(saved_stdin);
      saved_stdin = -1;
    }
    if (saved_stdout >= 0) {
      std::cout.flush();
      ::dup2(saved_stdout, STDOUT_FILENO);
      ::close(saved_stdout);
      saved_stdout = -1;
    }
    if (saved_stderr >= 0) {
      std::cerr.flush();
      ::dup2(saved_stderr, STDERR_FILENO);
      ::close(saved_stderr);
      saved_stderr = -1;
    }
  }

  bool wait_for_output(const std::string& text, int timeout_ms = 5000) {
    std::array<char, 4096> buf;
    std::string accumulated;

    auto start = std::chrono::steady_clock::now();
    while (true) {
      ssize_t n = ::read(master_fd, buf.data(), buf.size());
      if (n > 0) {
        accumulated.append(buf.data(), n);
        if (accumulated.find(text) != std::string::npos)
          return true;
      } else if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
        return false;
      }

      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
      if (elapsed >= timeout_ms) return false;

      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  void send_keys(const std::string& data) {
    size_t offset = 0;
    while (offset < data.size()) {
      ssize_t n = ::write(master_fd, data.c_str() + offset,
                          data.size() - offset);
      if (n > 0) {
        offset += n;
      } else if (n == -1 && errno == EAGAIN) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      } else {
        break;
      }
    }
  }

  void drain() {
    char buf[4096];
    while (::read(master_fd, buf, sizeof(buf)) > 0) {
      // discard
    }
  }

  int fd() const { return master_fd; }
};

inline bool drain_wait(std::atomic<bool>& app_exited, pty_terminal& pty,
                       int timeout_ms = 5000) {
  auto start = std::chrono::steady_clock::now();
  while (!app_exited.load(std::memory_order_acquire)) {
    pty.drain();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count();
    if (elapsed >= timeout_ms) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

// ---------------------------------------------------------------------------
// pty_drainer: background thread that continuously reads PTY master output
// into a buffer. Prevents the slave's output buffer from filling (which
// would block the app's writes) and provides wait_for() to check for
// expected rendered text.
// ---------------------------------------------------------------------------
struct pty_drainer {
  int fd_;
  std::mutex mutex_;
  std::string output_;
  std::atomic<bool> running_{true};
  std::thread thread_;

  explicit pty_drainer(int fd) : fd_(fd) {
    thread_ = std::thread([&]() {
      std::array<char, 4096> buf;
      while (running_.load(std::memory_order_acquire)) {
        ssize_t n = ::read(fd_, buf.data(), buf.size());
        if (n > 0) {
          std::lock_guard<std::mutex> lock(mutex_);
          output_.append(buf.data(), n);
        } else if (n == -1 && errno == EAGAIN) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } else {
          break;
        }
      }
    });
  }

  ~pty_drainer() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
  }

  bool wait_for(const std::string& text, int timeout_ms = 5000) {
    auto start = std::chrono::steady_clock::now();
    while (true) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (output_.find(text) != std::string::npos)
          return true;
      }
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
      if (elapsed >= timeout_ms) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    output_.clear();
  }
};

// ---------------------------------------------------------------------------
// app_cleanup: RAII guard that ensures the app thread is joined and the
// PTY is restored even if a REQUIRE macro throws (which would otherwise
// leave std::thread joinable, triggering std::terminate).
//
// Usage:
//   pty_test::pty_terminal pty;
//   std::atomic<bool> app_exited{false};
//   pty_test::pty_drainer drainer(pty.fd());
//   std::thread app_thread([&]() { ... app.run(root); ... });
//   pty_test::app_cleanup guard{pty, app_exited, app_thread};
//
//   // ... test assertions ...
//
//   // Normal shutdown:
//   pty.send_keys("q");
//   REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));
//   app_thread.join();
//   guard.joined = true;  // tell guard we handled cleanup
//   pty.restore();
//
// If a REQUIRE fails before guard.joined is set, the guard destructor
// sends Ctrl+C and 'q' to quit the app, drains the PTY, joins the thread,
// and restores stdio — preventing std::terminate.
// ---------------------------------------------------------------------------
struct app_cleanup {
  pty_terminal& pty;
  std::atomic<bool>& app_exited;
  std::thread& app_thread;
  bool joined = false;

  ~app_cleanup() {
    if (!joined && app_thread.joinable()) {
      // Try both Ctrl+C (mandatory exit in cpp-tui) and 'q' (common
      // registered quit key) to ensure the app exits.
      pty.send_keys("\x03");
      pty.send_keys("q");
      drain_wait(app_exited, pty, 3000);
      app_thread.join();
      pty.restore();
    }
  }
};

} // namespace pty_test

#endif // PTY_TEST_HARNESS_H