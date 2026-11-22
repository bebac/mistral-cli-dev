#include "doctest/doctest.h"
#include "dripcore/scheduler.h"
#include "dripcore/task.h"
#include "http/tcp_connect_task.h"
#include "http/tls_connection.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <system_error>

namespace
{
  // Loopback listener on an ephemeral port.
  class loopback_listener
  {
  public:
    loopback_listener()
    {
      fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
      if (fd_ < 0)
        throw std::runtime_error("socket failed");
      int yes = 1;
      setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      addr.sin_port = 0;
      if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        throw std::runtime_error("bind failed");
      if (::listen(fd_, 4) != 0)
        throw std::runtime_error("listen failed");
      socklen_t len = sizeof(addr);
      ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
      port_ = ntohs(addr.sin_port);
    }
    ~loopback_listener()
    {
      if (fd_ >= 0)
        ::close(fd_);
    }
    int fd_ = -1;
    uint16_t port_ = 0;
  };

  // A port on loopback that nothing listens on (bind, read the port,
  // close). Connecting to it is refused promptly and deterministically.
  uint16_t closed_loopback_port()
  {
    loopback_listener l;
    return l.port_; // listener closes on scope exit
  }
} // namespace

TEST_CASE("tcp_connect: connects to a loopback listener" * doctest::timeout(5))
{
  loopback_listener listener;
  dripcore::scheduler scheduler;
  std::atomic<bool> connected{false};

  class connect_task : public mistral::net::tcp_connect_task
  {
  public:
    connect_task(const mistral::net::address_info& addr, std::atomic<bool>& connected)
      : addr_(addr), connected_(connected)
    {
    }
    dripcore::awaitable<> body() override
    {
      auto sock = co_await tcp_connect(addr_);
      connected_ = sock.has_value();
      co_return;
    }
  private:
    const mistral::net::address_info& addr_;
    std::atomic<bool>& connected_;
  };

  auto addr = mistral::net::address_info::resolve("127.0.0.1", std::to_string(listener.port_));
  REQUIRE(addr.has_value());
  scheduler.spawn<connect_task>(*addr, connected);
  scheduler.run();

  CHECK(connected.load());
}

TEST_CASE("tcp_connect: refused connection throws promptly" * doctest::timeout(5))
{
  auto port = closed_loopback_port();
  dripcore::scheduler scheduler;
  std::atomic<bool> threw{false};
  std::atomic<int> err_code{0};

  class connect_task : public mistral::net::tcp_connect_task
  {
  public:
    connect_task(const mistral::net::address_info& addr, std::atomic<bool>& threw, std::atomic<int>& err)
      : addr_(addr), threw_(threw), err_(err)
    {
    }
    dripcore::awaitable<> body() override
    {
      try
      {
        co_await tcp_connect(addr_);
      }
      catch (const std::system_error& e)
      {
        err_ = e.code().value();
        threw_ = true;
      }
      co_return;
    }
  private:
    const mistral::net::address_info& addr_;
    std::atomic<bool>& threw_;
    std::atomic<int>& err_;
  };

  auto addr = mistral::net::address_info::resolve("127.0.0.1", std::to_string(port));
  REQUIRE(addr.has_value());

  auto start = std::chrono::steady_clock::now();
  scheduler.spawn<connect_task>(*addr, threw, err_code);
  scheduler.run();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start);

  CHECK(threw.load());
  CHECK(err_code.load() == ECONNREFUSED);
  CHECK(elapsed.count() < 1000); // refused, not hung
}

TEST_CASE("tcp_connect: cancel while connecting" * doctest::timeout(5))
{
  // 192.0.2.0/24 is TEST-NET-1 (RFC 5737): guaranteed non-routable, and
  // numeric so resolution needs no DNS. Without cancellation the connect
  // would sit out the kernel's SYN-retry timeout.
  dripcore::scheduler scheduler;
  std::atomic<bool> got_cancelled{false};

  class connect_task : public mistral::net::tcp_connect_task
  {
  public:
    connect_task(const mistral::net::address_info& addr, std::atomic<bool>& cancelled)
      : addr_(addr), cancelled_(cancelled)
    {
    }
    dripcore::awaitable<> body() override
    {
      try
      {
        co_await tcp_connect(addr_);
      }
      catch (const dripcore::cancelled&)
      {
        cancelled_ = true;
      }
      co_return;
    }
  private:
    const mistral::net::address_info& addr_;
    std::atomic<bool>& cancelled_;
  };

  class cancel_driver : public dripcore::task
  {
  public:
    cancel_driver(dripcore::task_control ctrl) : ctrl_(ctrl) {}
    dripcore::awaitable<> body() override
    {
      co_await sleep_for(std::chrono::milliseconds(50));
      ctrl_.cancel();
      co_return;
    }
  private:
    dripcore::task_control ctrl_;
  };

  auto addr = mistral::net::address_info::resolve("192.0.2.1", "80");
  REQUIRE(addr.has_value());

  auto start = std::chrono::steady_clock::now();
  auto ctrl = scheduler.spawn<connect_task>(*addr, got_cancelled);
  scheduler.spawn<cancel_driver>(ctrl);
  scheduler.run();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start);

  CHECK(got_cancelled.load());
  CHECK(ctrl.cancelled());
  CHECK(elapsed.count() < 2000); // cancelled promptly, not after the kernel SYN timeout
}