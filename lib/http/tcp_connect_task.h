#ifndef MISTRAL_TCP_CONNECT_TASK_H
#define MISTRAL_TCP_CONNECT_TASK_H

#include <dripcore/socket.h>
#include <dripcore/task.h>

#include <optional>

// Forward declarations keep this header free of the OpenSSL and
// tls_connection includes; the coroutine definitions live in
// tcp_connect_task.cpp. The typedef mirrors OpenSSL's own
// (typedef struct ssl_st SSL;) and is a legal identical redeclaration
// on either include order.
typedef struct ssl_st SSL;

namespace mistral::net
{
  class address_info;
  class tls_connection;

  // Task base that adds a fully non-blocking connect (TCP + TLS) to
  // dripcore::task.
  //
  // Deriving from this class (instead of dripcore::task directly) lets
  // the connect coroutines use the protected readable()/writable()
  // awaiters, so every suspension point reads
  //   co_await writable(sock);
  //   co_await readable(fd);
  // with io registration, cancellation, the once-only resume, and
  // shutdown deregistration all owned by dripcore's io_awaiter.
  //
  // The dripcore::socket is non-blocking from the start and never
  // leaves non-blocking mode: nothing in this path toggles blocking
  // modes. Definitions are in tcp_connect_task.cpp.
  class tcp_connect_task : public dripcore::task
  {
  protected:
    // Capability base, not a runnable task: nothing spawns a
    // tcp_connect_task directly. The no-op body() only keeps the class
    // concrete so non-spawned embedders (e.g. mcp_client) can use the
    // connect coroutines; runnable derived tasks override body().
    dripcore::awaitable<> body() override
    {
      co_return;
    }

    // Non-blocking TCP connect. Each resolved address is tried in
    // turn: the socket is created non-blocking, connect() proceeds
    // asynchronously (EINPROGRESS is expected and swallowed by
    // dripcore::socket::connect), and the task waits for the socket
    // to become writable — the connect finished (or failed) once it
    // is; the real result is collected with getsockopt(SO_ERROR). On
    // failure or cancellation the socket closes itself via RAII.
    //
    // There is no per-attempt deadline: an unreachable host is
    // bounded by the kernel's SYN-retry timeout, during which the
    // scheduler thread keeps running; task cancellation resumes
    // promptly with dripcore::cancelled.
    //
    // Returns the connected socket. The optional is only there
    // because dripcore's awaitable promise requires
    // default-constructible result types. Throws std::system_error
    // carrying the last errno if no address works.
    dripcore::awaitable<std::optional<dripcore::socket>> tcp_connect(const address_info& addr);

    // Non-blocking TLS handshake core: retries SSL_connect whenever it
    // reports SSL_ERROR_WANT_READ / SSL_ERROR_WANT_WRITE, waiting via
    // readable()/writable() on fd. The socket stays non-blocking.
    // Throws std::runtime_error on handshake failure.
    dripcore::awaitable<> tls_handshake(SSL* ssl, int fd);

    // TLS handshake over an attached connection (see
    // tls_connection::attach_connected), followed by h2 ALPN
    // verification.
    dripcore::awaitable<> tls_handshake(tls_connection& conn);
  };
} // namespace mistral::net

#endif // MISTRAL_TCP_CONNECT_TASK_H