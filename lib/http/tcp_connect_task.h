#ifndef MISTRAL_NB_CONNECT_H
#define MISTRAL_NB_CONNECT_H

#include "http/tls_connection.h" // for address_info

#include <dripcore/cancelled.h>
#include <dripcore/socket.h>
#include <dripcore/task.h>

#include <openssl/ssl.h>

#include <sys/socket.h>

#include <cerrno>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace mistral::net
{
  // Task base that adds a fully non-blocking connect (TCP + TLS) to
  // dripcore::task.
  //
  // Deriving from this class (instead of dripcore::task directly) lets
  // the connect coroutines use the protected readable()/writable()
  // awaiters, so every suspension point reads
  //   co_await writable(sock);
  //   co_await readable(conn);
  // with io registration, cancellation, the once-only resume, and
  // shutdown deregistration all owned by dripcore's io_awaiter.
  //
  // The dripcore::socket is non-blocking from the start and never
  // leaves non-blocking mode: nothing in this path toggles blocking
  // modes.
  class tcp_connect_task : public dripcore::task
  {
  protected:
    // Non-blocking TCP connect. Each resolved address is tried in
    // turn: the socket is created non-blocking, connect() proceeds
    // asynchronously (EINPROGRESS is expected and swallowed by
    // dripcore::socket::connect), and the task waits for the socket
    // to become writable — the connect finished (or failed) once it
    // is; the real result is collected with getsockopt(SO_ERROR). On
    // failure or cancellation the socket closes itself via RAII.
    //
    // There is no per-attempt deadline: an unreachable host is bounded
    // by the kernel's SYN-retry timeout, during which the scheduler
    // thread keeps running; task cancellation resumes promptly with
    // dripcore::cancelled.
    //
    // Returns the connected socket. The optional is only there because
    // dripcore's awaitable promise requires default-constructible
    // result types. Throws std::system_error carrying the last errno if
    // no address works.
    dripcore::awaitable<std::optional<dripcore::socket>> tcp_connect(const address_info& addr)
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

    // Non-blocking TLS handshake over the attached connection (see
    // tls_connection::attach_connected). The socket stays non-blocking:
    // SSL_connect is retried whenever it reports
    // SSL_ERROR_WANT_READ / SSL_ERROR_WANT_WRITE, waiting via
    // readable()/writable(). Throws on handshake failure or when the
    // negotiated ALPN is not 'h2'.
    dripcore::awaitable<> tls_handshake(tls_connection& conn)
    {
      SSL* ssl = conn.ssl();
      while (true)
      {
        int r = SSL_connect(ssl);
        if (r == 1)
        {
          break;
        }
        int err = SSL_get_error(ssl, r);
        if (err == SSL_ERROR_WANT_READ)
        {
          co_await readable(conn);
        }
        else if (err == SSL_ERROR_WANT_WRITE)
        {
          co_await writable(conn);
        }
        else
        {
          auto [msg, ec] = tls_connection::get_ssl_error_details();
          throw std::runtime_error(std::format("TLS handshake failed: {}", msg));
        }
      }
      conn.verify_alpn();
    }
  };
} // namespace mistral::net

#endif // MISTRAL_NB_CONNECT_H