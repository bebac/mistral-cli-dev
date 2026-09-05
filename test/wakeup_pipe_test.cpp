#include <doctest/doctest.h>
#include "common/wakeup_pipe.h"

#include <unistd.h>
#include <cerrno>
#include <atomic>
#include <chrono>
#include <thread>

TEST_CASE("wakeup_pipe construction creates valid pipe")
{
  mistral::common::wakeup_pipe pipe;
  REQUIRE(pipe.valid());
  REQUIRE(pipe.read_fd() >= 0);
  REQUIRE(pipe.write_fd() >= 0);
  REQUIRE(pipe.read_fd() != pipe.write_fd());
}

TEST_CASE("wakeup_pipe notify writes default byte that can be read")
{
  mistral::common::wakeup_pipe pipe;

  REQUIRE(pipe.notify());

  char buf = 0;
  ssize_t n = ::read(pipe.read_fd(), &buf, 1);
  REQUIRE(n == 1);
  REQUIRE(buf == '1');
}

TEST_CASE("wakeup_pipe notify with custom byte")
{
  mistral::common::wakeup_pipe pipe;

  REQUIRE(pipe.notify('X'));

  char buf = 0;
  ssize_t n = ::read(pipe.read_fd(), &buf, 1);
  REQUIRE(n == 1);
  REQUIRE(buf == 'X');
}

TEST_CASE("wakeup_pipe close_write causes EOF on read end")
{
  mistral::common::wakeup_pipe pipe;
  int read_fd = pipe.read_fd();

  pipe.close_write();

  char buf[16];
  ssize_t n = ::read(read_fd, buf, sizeof(buf));
  REQUIRE(n == 0);
  REQUIRE(!pipe.valid());
}

TEST_CASE("wakeup_pipe close_read and close_write individually")
{
  mistral::common::wakeup_pipe pipe;

  pipe.close_read();
  REQUIRE(pipe.read_fd() == -1);
  REQUIRE(pipe.write_fd() >= 0);

  pipe.close_write();
  REQUIRE(pipe.write_fd() == -1);
  REQUIRE(!pipe.valid());
}

TEST_CASE("wakeup_pipe destructor closes both ends")
{
  int read_fd = -1;
  int write_fd = -1;
  {
    mistral::common::wakeup_pipe pipe;
    read_fd = pipe.read_fd();
    write_fd = pipe.write_fd();
    REQUIRE(read_fd >= 0);
    REQUIRE(write_fd >= 0);
  }
  char buf = '1';
  ssize_t n = ::write(write_fd, &buf, 1);
  REQUIRE(n == -1);
  REQUIRE(errno == EBADF);
}

TEST_CASE("wakeup_pipe move constructor transfers ownership")
{
  mistral::common::wakeup_pipe pipe1;
  int read_fd = pipe1.read_fd();
  int write_fd = pipe1.write_fd();

  mistral::common::wakeup_pipe pipe2(std::move(pipe1));

  REQUIRE(pipe2.read_fd() == read_fd);
  REQUIRE(pipe2.write_fd() == write_fd);
  REQUIRE(pipe2.valid());

  REQUIRE(pipe1.read_fd() == -1);
  REQUIRE(pipe1.write_fd() == -1);
  REQUIRE(!pipe1.valid());

  REQUIRE(pipe2.notify());
  char buf = 0;
  ssize_t n = ::read(pipe2.read_fd(), &buf, 1);
  REQUIRE(n == 1);
  REQUIRE(buf == '1');
}

TEST_CASE("wakeup_pipe move assignment transfers ownership and closes old")
{
  mistral::common::wakeup_pipe pipe1;
  mistral::common::wakeup_pipe pipe2;
  int pipe1_read = pipe1.read_fd();
  int pipe2_read = pipe2.read_fd();

  pipe2 = std::move(pipe1);

  REQUIRE(pipe2.read_fd() == pipe1_read);
  REQUIRE(pipe2.valid());
  REQUIRE(!pipe1.valid());

  char buf[16];
  ssize_t n = ::read(pipe2_read, buf, sizeof(buf));
  REQUIRE(n == -1);
  REQUIRE(errno == EBADF);
}

TEST_CASE("wakeup_pipe release gives up ownership")
{
  mistral::common::wakeup_pipe pipe;
  int read_fd = pipe.read_fd();
  int write_fd = pipe.write_fd();

  auto [r, w] = pipe.release();
  REQUIRE(r == read_fd);
  REQUIRE(w == write_fd);
  REQUIRE(!pipe.valid());

  ::close(read_fd);
  ::close(write_fd);
}

TEST_CASE("wakeup_pipe notify after close_write returns false")
{
  mistral::common::wakeup_pipe pipe;

  pipe.close_write();
  REQUIRE(!pipe.notify());
}

TEST_CASE("wakeup_pipe close_write is idempotent")
{
  mistral::common::wakeup_pipe pipe;

  pipe.close_write();
  pipe.close_write();
  REQUIRE(pipe.write_fd() == -1);
}

TEST_CASE("wakeup_pipe cross-thread wakeup")
{
  mistral::common::wakeup_pipe pipe;
  std::atomic<bool> received{false};

  std::thread reader([&]() {
    char buf[16];
    ssize_t n = ::read(pipe.read_fd(), buf, sizeof(buf));
    if (n > 0) {
      received.store(true);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  pipe.notify();

  reader.join();
  REQUIRE(received.load());
}

TEST_CASE("wakeup_pipe multiple notifies can be drained")
{
  mistral::common::wakeup_pipe pipe;

  pipe.notify();
  pipe.notify();
  pipe.notify();

  char buf[16];
  ssize_t n = ::read(pipe.read_fd(), buf, sizeof(buf));
  REQUIRE(n == 3);
  REQUIRE(buf[0] == '1');
  REQUIRE(buf[1] == '1');
  REQUIRE(buf[2] == '1');
}

TEST_CASE("wakeup_pipe close_write signals EOF to blocked reader thread")
{
  mistral::common::wakeup_pipe pipe;
  std::atomic<bool> got_eof{false};

  std::thread reader([&]() {
    char buf[16];
    ssize_t n = ::read(pipe.read_fd(), buf, sizeof(buf));
    got_eof.store(n == 0);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  pipe.close_write();

  reader.join();
  REQUIRE(got_eof.load());
}