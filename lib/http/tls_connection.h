#ifndef MISTRAL_TLS_CONNECTION_H
#define MISTRAL_TLS_CONNECTION_H

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <dripcore/socket.h>

#include <netdb.h>
#include <unistd.h>

#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace mistral::net
{
  // RAII wrapper for getaddrinfo results.
  class address_info
  {
  public:
    static std::optional<address_info> resolve(std::string_view host, std::string_view port);

    ~address_info();

    address_info(address_info&& o) noexcept : info_(std::exchange(o.info_, nullptr))
    {
    }
    address_info& operator=(address_info&& o) noexcept;

    address_info(const address_info&) = delete;
    address_info& operator=(const address_info&) = delete;

    struct iterator
    {
      addrinfo* current;
      addrinfo* operator*() const
      {
        return current;
      }
      iterator& operator++()
      {
        if (current)
          current = current->ai_next;
        return *this;
      }
      bool operator!=(const iterator& o) const
      {
        return current != o.current;
      }
    };

    iterator begin() const
    {
      return iterator{info_};
    }
    iterator end() const
    {
      return iterator{nullptr};
    }

  private:
    explicit address_info(addrinfo* i) : info_(i)
    {
    }
    addrinfo* info_ = nullptr;
  };

  // RAII wrapper for SSL_CTX.
  class ssl_context
  {
  public:
    ssl_context();
    ~ssl_context();

    ssl_context(ssl_context&& o) noexcept : ctx_(std::exchange(o.ctx_, nullptr))
    {
    }
    ssl_context& operator=(ssl_context&& o) noexcept;

    ssl_context(const ssl_context&) = delete;
    ssl_context& operator=(const ssl_context&) = delete;

    void enable_h2();
    SSL_CTX* get() const
    {
      return ctx_;
    }

  private:
    SSL_CTX* ctx_ = nullptr;
  };

  // RAII wrapper for SSL.
  class ssl_handle
  {
  public:
    ssl_handle() = default;
    explicit ssl_handle(SSL_CTX* ctx);
    ~ssl_handle();

    ssl_handle(ssl_handle&& o) noexcept : ssl_(std::exchange(o.ssl_, nullptr))
    {
    }
    ssl_handle& operator=(ssl_handle&& o) noexcept;

    ssl_handle(const ssl_handle&) = delete;
    ssl_handle& operator=(const ssl_handle&) = delete;

    SSL* get() const
    {
      return ssl_;
    }
    void set_hostname(const std::string& host);
    void set_fd(int fd);
    int connect();
    std::string_view alpn_selected() const;

  private:
    SSL* ssl_ = nullptr;
  };

  // Error category for OpenSSL errors, so that std::error_code::message()
  // produces meaningful output instead of "unspecified system_category error".
  class ssl_error_category : public std::error_category
  {
  public:
    const char* name() const noexcept override;

    std::string message(int ev) const override;
  };

  // Returns the singleton ssl_error_category instance.
  const std::error_category& ssl_error_category_instance();

  // High-level TLS connection that owns a connected socket + SSL. The
  // socket is attached already connected and non-blocking (see
  // tcp_connect_task.h) and never toggles blocking modes; the TLS handshake
  // is driven there by tls_handshake() via readable()/writable().
  class tls_connection
  {
  public:
    tls_connection() = default;

    // Attach an already-connected non-blocking socket and prepare the
    // TLS handshake (h2 ALPN, SNI, SSL handle bound to the socket).
    // The handshake itself is driven by tls_handshake() in
    // tcp_connect_task.h — only a task can co_await readable()/writable().
    void attach_connected(dripcore::socket sock, std::string_view host);

    // Verify the ALPN negotiated by the completed handshake; throws
    // unless the server selected 'h2'.
    void verify_alpn();

    int fd() const
    {
      return sock_ ? sock_->platform_handle() : -1;
    }
    SSL* ssl() const
    {
      return ssl_.get();
    }
    bool connected() const
    {
      return sock_.has_value();
    }

    // Calling it platform_handle means we can call dripcore
    // readable/writable with a tls_connection instance.
    int platform_handle() const
    {
      return fd();
    }

    int read(std::span<std::byte> in)
    {
      return SSL_read(ssl_.get(), in.data(), static_cast<int>(in.size()));
    }
    int write(std::span<const std::byte> out)
    {
      return SSL_write(ssl_.get(), out.data(), static_cast<int>(out.size()));
    }
    bool no_data_pending() const
    {
      return SSL_pending(ssl_.get()) == 0;
    }

    bool is_shutting_down() const
    {
      return shutting_down_;
    }

    int shutdown()
    {
      // NOTE - Not sure about this. Thing is that dripcore has no really good way to stop
      // tasks that are waiting on IO. When using CTRL-C to stop, the http_read_task is very
      // likely to suspended on 'co_await readable(client)' and apparently closing the socket
      // won't notify it. Doing a shutdown does seem to wake it up though. I would have expected
      // SSL_shutdown to also do a shutdown on the fd, but that doesn't seem to work.
      // return SSL_shutdown(ssl_.get());
      shutting_down_ = true;
      if (sock_)
      {
        return ::shutdown(sock_->platform_handle(), SHUT_RDWR);
      }
      else
      {
        return 0;
      }
    }

    // Get error details from OpenSSL's error queue (for SSL_ERROR_SSL)
    static std::pair<std::string, std::error_code> get_ssl_error_details();

    tls_connection(tls_connection&&) = default;
    tls_connection& operator=(tls_connection&&) = default;
    tls_connection(const tls_connection&) = delete;
    tls_connection& operator=(const tls_connection&) = delete;

  private:
    ssl_context ssl_ctx_;
    ssl_handle ssl_;
    std::optional<dripcore::socket> sock_;
    bool shutting_down_ = false;
  };

  // Call once at process start.
  void init_openssl();

} // namespace mistral::net

#endif // MISTRAL_TLS_CONNECTION_H
