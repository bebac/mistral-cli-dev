#include "http/tcp_connect_task.h"

#include "http/tls_connection.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <sys/socket.h>

#include <cerrno>
#include <format>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace mistral::net
{
  dripcore::awaitable<std::optional<dripcore::socket>> tcp_connect_task::tcp_connect(const address_info& addr)
  {
    int last_err = 0;

    for (auto* res : addr)
    {
      try
      {
        dripcore::socket sock{res->ai_family, res->ai_socktype, res->ai_protocol};
        sock.nonblocking(true);
        sock.connect(res->ai_addr, res->ai_addrlen);
        co_await writable(sock);
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(sock.platform_handle(), SOL_SOCKET, SO_ERROR, &err, &len) != 0)
        {
          err = errno;
        }
        if (err == 0)
        {
          co_return std::move(sock);
        }
        last_err = err;
      }
      catch (const std::system_error& e)
      {
        // socket creation or the connect attempt failed; try the
        // next address. dripcore::cancelled is not a system_error
        // and propagates.
        last_err = e.code().value();
      }
    }

    if (last_err != 0)
    {
      throw std::system_error(last_err, std::system_category(), "connect");
    }
    throw std::runtime_error("failed to connect: no usable address");
  }

  dripcore::awaitable<> tcp_connect_task::tls_handshake(SSL* ssl, int fd)
  {
    while (true)
    {
      int r = SSL_connect(ssl);
      if (r == 1)
      {
        co_return;
      }
      int err = SSL_get_error(ssl, r);
      if (err == SSL_ERROR_WANT_READ)
      {
        co_await readable(fd);
      }
      else if (err == SSL_ERROR_WANT_WRITE)
      {
        co_await writable(fd);
      }
      else if (err == SSL_ERROR_ZERO_RETURN)
      {
        throw std::runtime_error("TLS handshake failed: peer closed the connection");
      }
      else
      {
        auto [msg, ec] = tls_connection::get_ssl_error_details();
        throw std::runtime_error(std::format("TLS handshake failed: {}", msg));
      }
    }
  }

  dripcore::awaitable<> tcp_connect_task::tls_handshake(tls_connection& conn)
  {
    co_await tls_handshake(conn.ssl(), conn.platform_handle());
    conn.verify_alpn();
  }
} // namespace mistral::net