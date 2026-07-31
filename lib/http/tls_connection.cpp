#include "http/tls_connection.h"

#include <sys/socket.h>
#include <netinet/tcp.h>
#include <format>
#include <iostream>

namespace mistral::net
{
  // ---- init_openssl ----

  void init_openssl()
  {
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
  }

  // ---- address_info ----

  std::optional<address_info> address_info::resolve(std::string_view host, std::string_view port)
  {
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string host_str(host);
    std::string port_str(port);

    addrinfo* result = nullptr;
    int s = getaddrinfo(host_str.c_str(), port_str.c_str(), &hints, &result);
    if (s != 0 || !result) {
      return std::nullopt;
    }
    return address_info(result);
  }

  address_info::~address_info()
  {
    if (info_) freeaddrinfo(info_);
  }

  address_info& address_info::operator=(address_info&& o) noexcept
  {
    if (this != &o) {
      if (info_) freeaddrinfo(info_);
      info_ = std::exchange(o.info_, nullptr);
    }
    return *this;
  }

  // ---- ssl_context ----

  ssl_context::ssl_context()
  {
    const SSL_METHOD* method = TLS_client_method();
    ctx_ = SSL_CTX_new(method);
    if (!ctx_) {
      throw std::runtime_error("unable to create SSL context");
    }
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
  }

  ssl_context::~ssl_context()
  {
    if (ctx_) SSL_CTX_free(ctx_);
  }

  ssl_context& ssl_context::operator=(ssl_context&& o) noexcept
  {
    if (this != &o) {
      if (ctx_) SSL_CTX_free(ctx_);
      ctx_ = std::exchange(o.ctx_, nullptr);
    }
    return *this;
  }

  void ssl_context::enable_h2()
  {
    const unsigned char protos[] = {2, 'h', '2'};
    SSL_CTX_set_alpn_protos(ctx_, protos, sizeof(protos));
  }

  // ---- ssl_handle ----

  ssl_handle::ssl_handle(SSL_CTX* ctx)
  {
    ssl_ = SSL_new(ctx);
    if (!ssl_) {
      throw std::runtime_error("unable to create SSL handle");
    }
  }

  ssl_handle::~ssl_handle()
  {
    if (ssl_) {
      SSL_shutdown(ssl_);
      SSL_free(ssl_);
    }
  }

  ssl_handle& ssl_handle::operator=(ssl_handle&& o) noexcept
  {
    if (this != &o) {
      if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); }
      ssl_ = std::exchange(o.ssl_, nullptr);
    }
    return *this;
  }

  void ssl_handle::set_hostname(const std::string& host)
  {
    SSL_set_tlsext_host_name(ssl_, host.c_str());
  }

  void ssl_handle::set_fd(int fd)
  {
    SSL_set_fd(ssl_, fd);
  }

  int ssl_handle::connect()
  {
    return SSL_connect(ssl_);
  }

  void ssl_handle::set_non_blocking(bool enable)
  {
    int fd = SSL_get_fd(ssl_);
    int flags = fcntl(fd, F_GETFL, 0);
    if (enable) {
      fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    } else {
      fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
  }

  std::string_view ssl_handle::alpn_selected() const
  {
    if (!ssl_) return {};
    const unsigned char* data = nullptr;
    unsigned int len = 0;
    SSL_get0_alpn_selected(ssl_, &data, &len);
    if (data && len > 0) {
      return {reinterpret_cast<const char*>(data), len};
    }
    return {};
  }

  // ---- tls_connection ----

  tls_connection::~tls_connection()
  {
    if (sock_ >= 0) {
      ::close(sock_);
      sock_ = -1;
    }
  }

  int tls_connection::create_tcp_socket(std::string_view host, std::string_view port)
  {
    auto addr = address_info::resolve(host, port);
    if (!addr) {
      throw std::runtime_error(std::format("failed to resolve {}:{}", host, port));
    }

    for (auto* res : *addr) {
      int sock = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
      if (sock == -1) continue;

      if (::connect(sock, res->ai_addr, res->ai_addrlen) == 0)
      {
        // Disable Nagle's algorithm — critical for HTTP/2 where we send
        // small control frames (WINDOW_UPDATE, SETTINGS_ACK) that must
        // not be delayed.
        int yes = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

#if defined(__APPLE__)
        int value = 1;
        if (setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &value, sizeof(value)) < 0) {
          std::cerr << "[http] Failed to enable SO_NOSIGPIPE" << std::endl;
        }
#else
        // TODO
#endif
        return sock;
      }
      ::close(sock);
    }

    throw std::runtime_error(std::format("failed to connect to {}:{}", host, port));
  }

  void tls_connection::connect(std::string_view host, std::string_view port)
  {
    ssl_ctx_.enable_h2();

    sock_ = create_tcp_socket(host, port);

    ssl_ = ssl_handle(ssl_ctx_.get());

    std::string host_str(host);
    ssl_.set_hostname(host_str);
    ssl_.set_fd(sock_);

    if (ssl_.connect() <= 0) {
      throw std::runtime_error("TLS handshake failed");
    }

    auto alpn = ssl_.alpn_selected();
    if (alpn != "h2") {
      throw std::runtime_error(std::format("expected ALPN 'h2', got '{}'", alpn));
    }
  }

  std::pair<std::string, std::error_code> tls_connection::get_ssl_error_details()
  {
    unsigned long ssl_err = ERR_get_error();
    if (ssl_err == 0) {
      // Fallback to errno if no SSL error in queue
      return {
        "SSL error (no details)",
        std::make_error_code(static_cast<std::errc>(errno))
      };
    }
    else {
      char err_buf[256];
      ERR_error_string_n(ssl_err, err_buf, sizeof(err_buf));
      return {
        std::string("SSL error: ") + err_buf,
        std::error_code(static_cast<int>(ssl_err), std::system_category())
      };
    }
  }

} // namespace mistral::http
