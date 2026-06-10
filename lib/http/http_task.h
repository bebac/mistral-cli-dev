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

#include <charconv>
#include <format>
#include <iostream>
#include <memory>
#include <string>

namespace mistral::net
{
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
      // Blocking connect + TLS handshake (V1: happens once).
      try {
        conn_.connect(host_, std::to_string(port_));
      } catch (const std::exception& e) {
        std::cerr << "[http_task] connection failed: " << e.what() << std::endl;
        response_ch_->try_send({api::event_type::error, e.what()});
        response_ch_->close();
        co_return;
      }

      SSL* ssl = conn_.ssl();
      int fd = conn_.fd();

      // Process requests until channel closes.
      while (true)
      {
        auto req = co_await request_ch_->recv();
        if (!req) break;

        // --- Process a single HTTP request ---

        std::cout << "[http_task] Sending request of size: " << req->body.size() << std::endl;

        bool stream_done = false;
        int status_code = 0;
        std::string response_body;
        sse_parser_.reset();

        auto stream = context_.open_stream();

        stream.on_headers([&](const ::http::headers& h) {
          auto status = h.get(":status");
          if (!status.empty()) {
            std::from_chars(status.data(), status.data() + status.size(), status_code);
          }
        });

        stream.on_data([&](std::span<const std::byte> data) {
          std::string_view chunk(reinterpret_cast<const char*>(data.data()), data.size());

          if (req->stream) {
            auto events = sse_parser_.feed(chunk);
            for (auto& evt : events) {
              if (evt.kind == api::sse_parser::event::type::done) {
                response_ch_->try_send({api::event_type::done, {}});
              } else {
                response_ch_->try_send({api::event_type::data, std::move(evt.payload)});
              }
            }
          } else {
            response_body.append(chunk);
          }
        });

        stream.on_end([&]() {
          if (!req->stream) {
            if (status_code >= 200 && status_code < 300) {
              response_ch_->try_send({api::event_type::data, std::move(response_body)});
            } else {
              response_ch_->try_send({api::event_type::error,
                std::format("HTTP {}: {}", status_code, response_body)});
            }
            response_ch_->try_send({api::event_type::done, {}});
          }
          stream_done = true;
        });

        stream.on_reset([&](std::error_code ec) {
          response_ch_->try_send({api::event_type::error,
            std::format("stream reset: {}", ec.message())});
          response_ch_->try_send({api::event_type::done, {}});
          stream_done = true;
        });

        ::http::headers hdrs{
          {"content-type",  "application/json"},
          {"authorization", "Bearer " + api_key_},
          {"accept",        req->stream ? "text/event-stream" : "application/json"},
          {"user-agent",    "mistral-cli/1.0"},
        };

        stream.send_headers("POST", req->path, host_, hdrs, false);

        auto body_bytes = std::as_bytes(std::span{req->body.data(), req->body.size()});
        stream.send_data(body_bytes, true);

        // --- SSL pump loop ---
        //
        // Key subtlety: SSL_read may consume data from OpenSSL's internal
        // TLS record buffer, not just from the kernel socket buffer.
        // After a successful SSL_read, there may be more data available
        // in the SSL layer that won't trigger a kqueue readable event.
        // We must check SSL_pending() before co_await readable(fd).

        while (!stream_done)
        {
          // 1. Flush pending output (WINDOW_UPDATE, SETTINGS_ACK, etc.)
          //    This must happen promptly — HTTP/2 flow control depends on it.
          while (true) {
            auto out = context_.output_begin();
            if (out.empty()) break;

            int n = SSL_write(ssl, out.data(), static_cast<int>(out.size()));

            if (n > 0) {
              context_.output_end(n);
            } else {
              int err = SSL_get_error(ssl, n);
              if (err == SSL_ERROR_WANT_WRITE) {
                co_await writable(fd);
              } else if (err == SSL_ERROR_WANT_READ) {
                co_await readable(fd);
              } else {
                stream_done = true;
                break;
              }
            }
          }

          if (stream_done) break;

          // 2. Read incoming data — drain all available SSL data.
          //    Only block on the fd if SSL has no buffered data.
          if (SSL_pending(ssl) == 0) {
            co_await readable(fd);
          }

          // Drain loop: process all available SSL records before breaking
          // to flush. This avoids N outer-loop iterations for N records
          // in a burst, and keeps the response channel fed without gaps.
          // WINDOW_UPDATEs accumulate in the output buffer and get flushed
          // in one batch after the drain — safe because the flow control
          // window (65KB) is far larger than a typical SSL burst.
          while (!stream_done) {
            auto in = context_.input_begin();
            if (in.empty()) break;

            int n = SSL_read(ssl, in.data(), static_cast<int>(in.size()));

            if (n > 0) {
              context_.input_end(n);  // Processes HTTP/2 frames, fires callbacks.
              if (SSL_pending(ssl) == 0) break;  // No more buffered data.
              // Otherwise continue draining.
            }
            else
            {
              int err = SSL_get_error(ssl, n);
              if (err == SSL_ERROR_WANT_READ) {
                break;  // No more data — go back to outer loop.
              } else if (err == SSL_ERROR_WANT_WRITE) {
                co_await writable(fd);
                break;  // Retry from outer loop.
              } else if (err == SSL_ERROR_ZERO_RETURN) {
                stream_done = true;
              }
              else
              {
                response_ch_->try_send({api::event_type::error, std::format("stream closed: {}", err)});
                response_ch_->try_send({api::event_type::done, {}});
                stream_done = true;
              }
            }
          }
        }

        // Ensure done event was sent.
        if (req->stream && !stream_done) {
          response_ch_->try_send({api::event_type::done, {}});
        }
      }

      std::cerr << "[http_task] closing!" << std::endl;

      response_ch_->close();
      co_return;
    }

  private:
    std::string host_;
    int port_;
    std::string api_key_;

    http::tls_connection conn_;
    ::http::client_context context_;
    api::sse_parser sse_parser_;

    std::shared_ptr<dripcore::channel<api::api_request>> request_ch_;
    std::shared_ptr<dripcore::channel<api::api_event>> response_ch_;
  };

} // namespace mistral::net

#endif // MISTRAL_HTTP_TASK_H
