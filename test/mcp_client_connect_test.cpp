#include "doctest/doctest.h"
#include "mcp/mcp_client.h"

#include <dripcore/scheduler.h>
#include <dripcore/task.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>

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

TEST_CASE("mcp_client: connects to a loopback listener (http)" * doctest::timeout(5))
{
  loopback_listener listener;
  dripcore::scheduler scheduler;
  std::atomic<bool> connected{false};

  class connect_task : public dripcore::task
  {
  public:
    connect_task(const std::string& url, std::atomic<bool>& connected)
      : url_(url), connected_(connected)
    {
    }
    dripcore::awaitable<> body() override
    {
      mistral::mcp::mcp_client client(url_, std::chrono::seconds{5});
      connected_ = co_await client.connect();
      co_return;
    }
  private:
    std::string url_;
    std::atomic<bool>& connected_;
  };

  scheduler.spawn<connect_task>(
    "http://127.0.0.1:" + std::to_string(listener.port_) + "/mcp", connected);
  scheduler.run();

  CHECK(connected.load());
}

TEST_CASE("mcp_client: refused connection fails promptly" * doctest::timeout(5))
{
  auto port = closed_loopback_port();
  dripcore::scheduler scheduler;
  std::atomic<bool> connected{true};
  std::atomic<bool> has_error{false};

  class connect_task : public dripcore::task
  {
  public:
    connect_task(const std::string& url, std::atomic<bool>& connected, std::atomic<bool>& has_error)
      : url_(url), connected_(connected), has_error_(has_error)
    {
    }
    dripcore::awaitable<> body() override
    {
      mistral::mcp::mcp_client client(url_, std::chrono::seconds{5});
      connected_ = co_await client.connect();
      has_error_ = !client.last_connect_error().empty();
      co_return;
    }
  private:
    std::string url_;
    std::atomic<bool>& connected_;
    std::atomic<bool>& has_error_;
  };

  auto start = std::chrono::steady_clock::now();
  scheduler.spawn<connect_task>(
    "http://127.0.0.1:" + std::to_string(port) + "/mcp", connected, has_error);
  scheduler.run();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start);

  CHECK(!connected.load());
  CHECK(has_error.load());
  CHECK(elapsed.count() < 1000); // refused, not hung
}