#ifndef MISTRAL_TLS_CONNECTION_H
#define MISTRAL_TLS_CONNECTION_H

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>

#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mistral::net
{
  // RAII wrapper for getaddrinfo results.
  class address_info
  {
  public:
    static std::optional<address_info> resolve(std::string_view host, std::string_view port);

    ~address_info();

    address_info(address_info&& o) noexcept : info_(std::exchange(o.info_, nullptr)) {}
    address_info& operator=(address_info&& o) noexcept;

    address_info(const address_info&) = delete;
    address_info& operator=(const address_info&) = delete;

    struct iterator
    {
      addrinfo* current;
      addrinfo* operator*() const { return current; }
      iterator& operator++() { if (current) current = current->ai_next; return *this; }
      bool operator!=(const iterator& o) const { return current != o.current; }
    };

    iterator begin() const { return iterator{info_}; }
    iterator end() const { return iterator{nullptr}; }

  private:
    explicit address_info(addrinfo* i) : info_(i) {}
    addrinfo* info_ = nullptr;
  };

  // RAII wrapper for SSL_CTX.
  class ssl_context
  {
  public:
    ssl_context();
    ~ssl_context();

    ssl_context(ssl_context&& o) noexcept : ctx_(std::exchange(o.ctx_, nullptr)) {}
    ssl_context& operator=(ssl_context&& o) noexcept;

    ssl_context(const ssl_context&) = delete;
    ssl_context& operator=(const ssl_context&) = delete;

    void enable_h2();
    SSL_CTX* get() const { return ctx_; }

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

    ssl_handle(ssl_handle&& o) noexcept : ssl_(std::exchange(o.ssl_, nullptr)) {}
    ssl_handle& operator=(ssl_handle&& o) noexcept;

    ssl_handle(const ssl_handle&) = delete;
    ssl_handle& operator=(const ssl_handle&) = delete;

    SSL* get() const { return ssl_; }
    void set_hostname(const std::string& host);
    void set_fd(int fd);
    int connect();
    void set_non_blocking(bool enable);
    std::string_view alpn_selected() const;

  private:
    SSL* ssl_ = nullptr;
  };

  // High-level TLS connection that manages TCP socket + SSL.
  // Performs blocking connect and TLS handshake, then switches to non-blocking.
  class tls_connection
  {
  public:
    tls_connection() = default;

    // Connect to host:port with TLS + HTTP/2 ALPN.
    void connect(std::string_view host, std::string_view port);

    int fd() const { return sock_; }
    SSL* ssl() const { return ssl_.get(); }
    bool connected() const { return sock_ >= 0; }

    void set_non_blocking(bool enable) { ssl_.set_non_blocking(enable); }

    int read(std::span<std::byte> in) {
        return SSL_read(ssl_.get(), in.data(), static_cast<int>(in.size()));
    }
    int write(std::span<const std::byte> out) {
        return SSL_write(ssl_.get(), out.data(), static_cast<int>(out.size()));
    }
    bool no_data_pending() const {
        return SSL_pending(ssl_.get()) == 0;
    }

    ~tls_connection();

    tls_connection(tls_connection&&) = default;
    tls_connection& operator=(tls_connection&&) = default;
    tls_connection(const tls_connection&) = delete;
    tls_connection& operator=(const tls_connection&) = delete;

  private:
    int create_tcp_socket(std::string_view host, std::string_view port);

    ssl_context ssl_ctx_;
    ssl_handle ssl_;
    int sock_ = -1;
  };

  // Call once at process start.
  void init_openssl();

} // namespace mistral::http

#endif // MISTRAL_TLS_CONNECTION_H
