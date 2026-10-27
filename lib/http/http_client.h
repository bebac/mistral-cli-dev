#ifndef MISTRAL_HTTP_CLIENT_H
#define MISTRAL_HTTP_CLIENT_H

#include "http/tls_connection.h"

#include <http/client_context.h>

#include <memory>
#include <openssl/ssl.h>
#include <span>

namespace mistral::net
{
  // Helper class that ties together the HTTP client context and the TLS connection. These
  // are thight coupled as a new connection means we also need a new context as the context
  // holds the protocol engine which will handle sending preface etc. on connection init.
  class http_client
  {
  public:
    http_client()
    {
      reset();
    }

    void reset()
    {
      conn_ = std::make_unique<net::tls_connection>();
      context_ = std::make_unique<http::client_context>();
    }

    // Calling it platform_handle means we can call dripcore readable/writable with a http_client instance.
    int platform_handle() const
    {
      return conn_->fd();
    }

    bool connected() const
    {
      return conn_->connected();
    }

    void connect(std::string host, int port)
    {
      // NOTE: Connect is blocking for now, should probably use non-blocking connect as well.
      conn_->connect(host, std::to_string(port));
      // Switch to non-blocking mode for dripcore event loop
      conn_->set_non_blocking(true);
    }

    int shutdown()
    {
      return conn_->shutdown();
    }

    bool is_shutting_down() const
    {
      return conn_->is_shutting_down();
    }

    auto open_stream()
    {
      return context_->open_stream();
    }

    auto get_stream(uint32_t id)
    {
      return context_->get_stream(id);
    }

    auto active_streams()
    {
      return context_->active_streams();
    }

    void set_initial_window_size(uint32_t size)
    {
      context_->set_initial_window_size(size);
    }

    // Event draining (async event interface)
    bool has_events() const
    {
      return context_->has_events();
    }

    auto next_event()
    {
      return context_->next_event();
    }

    auto input_begin()
    {
      return context_->input_begin();
    }

    void input_end(size_t n)
    {
      context_->input_end(n);
    }

    auto output_begin()
    {
      return context_->output_begin();
    }

    void output_end(size_t n)
    {
      context_->output_end(n);
    }

    void on_output_ready(std::function<void()> cb)
    {
      context_->on_output_ready(std::move(cb));
    }

    int read(std::span<std::byte> in)
    {
      return conn_->read(in);
    }

    int write(std::span<const std::byte> out)
    {
      return conn_->write(out);
    }

    bool no_data_pending()
    {
      return conn_->no_data_pending();
    }

    int get_error(int n)
    {
      return SSL_get_error(conn_->ssl(), n);
    }

  private:
    std::unique_ptr<net::tls_connection> conn_;
    std::unique_ptr<http::client_context> context_;
  };
} // namespace mistral::net

#endif // MISTRAL_HTTP_CLIENT_H
