#ifndef MISTRAL_HTTP_TASK_H
#define MISTRAL_HTTP_TASK_H

#include "http/tls_connection.h"
#include "api/mistral_types.h"
#include "api/sse_parser.h"

#include <dripcore/task.h>
#include <dripcore/channel.h>
#include <dripcore/coroutine.h>

#include <http/client_context.h>
#include <http/headers.h>
#include <http/stream.h>

#include <openssl/ssl.h>

#include <fcntl.h>
#include <unistd.h>
#include <charconv>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

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

    int fd()
    {
      return conn_->fd();
    }

    void connect(std::string host, int port)
    {
      // NOTE: Connect is blocking for now, should probably use nonblock connect as well.
      conn_->connect(host, std::to_string(port));
      // Switch to non-blocking mode for dripcore event loop
      conn_->set_non_blocking(true);
    }

    auto open_stream() { return context_->open_stream(); }

    auto input_begin() { return context_->input_begin(); }
    void input_end(size_t n) { context_->input_end(n); }
    auto output_begin() { return context_->output_begin(); }
    void output_end(size_t n) { context_->output_end(n); }

    int read(std::span<std::byte> in) { return SSL_read(conn_->ssl(), in.data(), static_cast<int>(in.size())); }
    int write(std::span<const std::byte> out) { return SSL_write(conn_->ssl(), out.data(), static_cast<int>(out.size())); }
    bool no_data_pending() { return SSL_pending(conn_->ssl()) == 0; }
    int get_error(int n) { return SSL_get_error(conn_->ssl(), n); }

  private:
    std::unique_ptr<net::tls_connection> conn_;
    std::unique_ptr<http::client_context> context_;
  };

  // HTTP task. Pulls request from request channel and stream response to reponse channel.

  class http_task : public dripcore::task
  {
  public:
    http_task(
      std::string host,
      int port,
      std::string api_key,
      std::shared_ptr<dripcore::channel<api::api_request>> request_ch,
      std::shared_ptr<dripcore::channel<api::api_event>> response_ch
    )
      : host_(std::move(host))
      , port_(port)
      , api_key_(std::move(api_key))
      , request_ch_(std::move(request_ch))
      , response_ch_(std::move(response_ch))
    {}

    dripcore::coroutine execute()
    {
      // Process requests until channel closes.
      while (true)
      {
        auto req = co_await request_ch_->recv();

        // No request means we are shutting down.
        if (!req) break;

        // Reset state for new request.
        response_.reset();
        sse_parser_.reset();

        unsigned retries = 0;

        while ( !response_.done && ++retries < 3 )
        {
          // If not connected, establish connection or end with an error.
          if ( !connected_ )
          {
            // Reset client to create a new HTTP client context and tls connection.
            client_.reset();
            try
            {
              std::cerr << "[http_task] connecting (" << retries << ")..." << std::endl;
              client_.connect(host_, port_);
              connected_ = true;
            }
            catch (const std::exception& e)
            {
              std::cerr << "[http_task] initial connection failed: " << e.what() << std::endl;
              response_ch_->try_send({api::event_type::error, e.what()});
              response_ch_->close();
              co_return;
            }
          }

          // --- Process a single HTTP request ---

          std::cout << "[http_task] Sending request of size: " << req->body.size() << std::endl;

          // Open a new HTTP stream for this request.
          auto stream = open_stream(client_, *req);

          // Buffer request (headers + body).
          init_request(stream, *req);

          // --- SSL pump loop ---
          //
          // Key subtlety: SSL_read may consume data from OpenSSL's internal
          // TLS record buffer, not just from the kernel socket buffer.
          // After a successful SSL_read, there may be more data available
          // in the SSL layer that won't trigger a kqueue readable event.
          // We must check SSL_pending() before co_await readable(fd).

          while (!response_.done)
          {
            while (true)
            {
              auto out = client_.output_begin();

              // init_request buffers the complete request, so once we get an empty output buffer we
              // have sent the complete request.
              if (out.empty())
                break;

              int n = client_.write(out);

              if (n > 0) {
                client_.output_end(n);
              }
              else {
                int err = client_.get_error(n);
                if (err == SSL_ERROR_WANT_WRITE) {
                  co_await writable(client_.fd());
                }
                else if (err == SSL_ERROR_WANT_READ) {
                  co_await readable(client_.fd());
                }
                else {
                  std::cerr << "[http_task] SSL_write error " << err << std::endl;
                  response_.done = true;
                  response_ch_->try_send({api::event_type::error, std::format("SSL write error: {}",  err)});
                  response_ch_->try_send({api::event_type::done, {}});
                  std::this_thread::sleep_for(std::chrono::milliseconds(2000)); // TEMP! dripcore does not currently have a sleep/timer support.
                }
              }
            }

            if (response_.done) break;

            // Read incoming data — drain all available SSL data.
            // Only block on the fd if SSL has no buffered data.
            if (client_.no_data_pending()) {
              co_await readable(client_.fd());
            }

            while (!response_.done)
            {
              auto in = client_.input_begin();

              if (in.empty())
                break;

              int n = client_.read(in);

              if (n > 0) {
                client_.input_end(n);  // Processes HTTP/2 frames, fires callbacks.
              }
              else {
                int err = client_.get_error(n);

                if (err == SSL_ERROR_WANT_READ) {
                  break;
                }
                else if (err == SSL_ERROR_WANT_WRITE) {
                  co_await writable(client_.fd());
                }
                else {
                  std::cerr << "[http_task] stream closed" << std::endl;
                  connected_ = false;
                  break;
                }
              }
            }

            if (!connected_ && !response_.done) {
              // Break to outer loop to retry connect.
              break;
            }
          }
        }

        // Ensure done event was sent.
        if (req->stream && !response_.done) {
          response_ch_->try_send({api::event_type::done, {}});
        }
      }

      std::cerr << "[http_task] closing!" << std::endl;

      response_ch_->close();
      co_return;
    }

  private:
    auto open_stream(http_client& client, const api::api_request& req) -> ::http::stream
    {
      auto stream = client.open_stream();

      stream.on_headers(
        [&](const ::http::headers& h) {
          auto status = h.get(":status");
          if (!status.empty()) {
            std::from_chars(status.data(), status.data() + status.size(), response_.status_code);
          }
        }
      );

      stream.on_data(
        [&](std::span<const std::byte> data) {
          std::string_view chunk(reinterpret_cast<const char*>(data.data()), data.size());

          if (req.stream) {
            auto events = sse_parser_.feed(chunk);
            for (auto& evt : events) {
              if (evt.kind == api::sse_parser::event::type::done) {
                response_ch_->try_send({api::event_type::done, {}});
              } else {
                response_ch_->try_send({api::event_type::data, std::move(evt.payload)});
              }
            }
          } else {
            response_.body.append(chunk);
          }
        }
      );

      stream.on_end(
        [&]() {
          if (!req.stream) {
            if (response_.status_code >= 200 && response_.status_code < 300) {
              response_ch_->try_send({api::event_type::data, std::move(response_.body)});
            } else {
              response_ch_->try_send({api::event_type::error,
                std::format("HTTP {}: {}", response_.status_code, response_.body)});
            }
            response_ch_->try_send({api::event_type::done, {}});
          }
          response_.done = true;
        }
      );

      stream.on_reset(
        [&](std::error_code ec) {
          response_ch_->try_send({api::event_type::error,
            std::format("stream reset: {}", ec.message())});
          response_ch_->try_send({api::event_type::done, {}});
          response_.done = true;
        }
      );

      return stream;
    }

    void init_request(http::stream& stream, const api::api_request& req)
    {
      ::http::headers hdrs{
        {"content-type",  "application/json"},
        {"authorization", "Bearer " + api_key_},
        {"accept",        req.stream ? "text/event-stream" : "application/json"},
        {"user-agent",    "mistral-cli/1.0"},
      };

      stream.send_headers(req.method, req.path, host_, hdrs, false);

      auto body_bytes = std::as_bytes(std::span{req.body.data(), req.body.size()});
      stream.send_data(body_bytes, true);
    }

    std::string host_;
    int port_;
    std::string api_key_;

    http_client client_;
    api::sse_parser sse_parser_;
    api::api_response response_;
    bool connected_ = false;

    std::shared_ptr<dripcore::channel<api::api_request>> request_ch_;
    std::shared_ptr<dripcore::channel<api::api_event>> response_ch_;
  };
} // namespace mistral::net

#endif // MISTRAL_HTTP_TASK_H