#ifndef MISTRAL_HTTP_TASK_H
#define MISTRAL_HTTP_TASK_H

#include "http/http_client.h"
#include "http/tls_connection.h"
#include "api/mistral_types.h"
#include "trace/trace.h"

#include <dripcore/task.h>
#include <dripcore/channel.h>
#include <dripcore/task_handle.h>
#include <dripcore/awaitable.h>

#include <http/headers.h>
#include <http/stream.h>
#include <http/sse.h>

#include <openssl/ssl.h>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <charconv>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace mistral::net
{
  // HTTP task. Pulls requests from request channel and stream response to response channel.

  class http_task : public dripcore::task
  {
  public:
    http_task(
      std::string host,
      int port,
      std::string api_key,
      std::shared_ptr<dripcore::channel<api::api_request>> request_ch
    )
      : host_(std::move(host))
      , port_(port)
      , api_key_(std::move(api_key))
      , request_ch_(std::move(request_ch))
    {
      // Register connection-level callbacks
      client_.on_goaway([this](uint32_t last_stream_id, std::error_code ec) {
        std::cerr << "[http_task] GOAWAY received, last_stream_id=" << last_stream_id
                  << ", error=" << ec.message() << std::endl;
        connected_ = false;
      });

      client_.on_connection_error([this](std::error_code ec) {
        std::cerr << "[http_task] Connection error: " << ec.message() << std::endl;
        connected_ = false;
      });
    }

  public:
    dripcore::task_handle execute()
    {
      // Process requests until channel closes.
      while (true)
      {
        auto req = co_await request_ch_->recv();

        // No request means we are shutting down.
        if (!req) break;

        auto response_ch = req->response_ch;

        if ( !response_ch ) {
          std::cerr << "[http_task] response_ch is required!" << std::endl;
          continue;
        }

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
              std::cerr << "[http_task] " << host_ << " connecting (" << retries << ")..." << std::endl;
              client_.connect(host_, port_);
              connected_ = true;
            }
            catch (const std::exception& e)
            {
              std::cerr << "[http_task] initial connection failed: " << e.what() << std::endl;
              response_ch->try_send({api::event_type::error, e.what()});
              response_ch->close();
              co_return;
            }
          }

          // --- Process a single HTTP request ---

          std::cout << "[http_task] Sending request " << req->path << ", size: " << req->body.size() << std::endl;

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
            if (auto err = co_await client_output_task(connected_))
            {
              response_.done = true;
              response_ch->try_send({api::event_type::error, *err});
            }

            if (!connected_)
              break;

            if (response_.done)
              break;

            while (!response_.done)
            {
              if (auto err = co_await client_output_task(connected_)) {
                std::cerr << "[http_task] client_output_task error: " << *err << std::endl;
              }

              if ( !connected_ ) {
                break;
              }

              if (auto err = co_await client_input_task(connected_)) {
                std::cerr << "[http_task] client_input_task error: " << *err << std::endl;
                break;
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
          response_ch->try_send({api::event_type::done, {}});
        }
      }

      std::cerr << "[http_task] closing!" << std::endl;

      co_return;
    }

  private:
    dripcore::awaitable<std::optional<std::string>> client_output_task(bool& connected)
    {
      std::optional<std::string> error;

      if (!connected) {
        std::cerr << "[http_task] client_output_task !connected" << std::endl;
      }

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
        else
        {
          int err = client_.get_error(n);

          if (err == SSL_ERROR_WANT_WRITE) {
            co_await writable(client_.platform_handle());
          }
          else if (err == SSL_ERROR_WANT_READ) {
            co_await readable(client_.platform_handle());
          }
          else if (err == SSL_ERROR_SYSCALL) {
            // Connection closed or reset - not a fatal error, allows retry
            if (errno == 0 || errno == EPIPE || errno == ECONNRESET) {
              std::cerr << "[http_task] SSL_write: connection closed error: " << errno << "(" <<strerror(errno) << ")" << std::endl;
              connected = false;
              break;
            }
            else {
              co_await sleep_for(std::chrono::milliseconds(500));
              error = std::format("[http_task] SSL_write: syscall error: {} ({})", errno, strerror(errno));
              break;
            }
          }
          else {
            co_await sleep_for(std::chrono::milliseconds(500));
            error = std::format("[http_task] SSL_write: error: {}", err);
            break;
          }
        }
      }
      co_return error;
    }

    dripcore::awaitable<std::optional<std::string>> client_input_task(bool& connected)
    {
      std::optional<std::string> error;

      auto in = client_.input_begin();

      if (in.empty()) {
        co_return error;
      }

      int n = client_.read(in);

      if (n > 0)
      {
        mistral::trace::logger::instance().trace(mistral::trace::category::data, "read {}", in.subspan(0, n));
        client_.input_end(n);  // Processes HTTP/2 frames, fires callbacks.
      }
      else
      {
        int err = client_.get_error(n);

        if (err == SSL_ERROR_WANT_READ)
        {
          // Only wait on fd if SSL has no pending data.
          if (client_.no_data_pending()) {
            co_await readable(client_.platform_handle());
          }
        }
        else if (err == SSL_ERROR_WANT_WRITE) {
          co_await writable(client_.platform_handle());
        }
        else if (err == SSL_ERROR_ZERO_RETURN) {
          // Connection closed gracefully by peer
          error = "[http_task] SSL_read: connection closed gracefully";
          connected = false;
        }
        else if (err == SSL_ERROR_SYSCALL)
        {
          if (errno == 0 || errno == ECONNRESET) {
            error = std::format("[http_task] SSL_read: connection closed (errno={})", errno);
            connected = false;
          }
          else {
            error = std::format("[http_task] SSL_read: syscall error errno={}", errno);
            connected = false;
          }
        }
        else {
          error = std::format("[http_task] stream closed err={}", err);
          connected = false;
        }
      }

      co_return error;
    }

  private:
    auto open_stream(http_client& client, const api::api_request& req) -> ::http::stream
    {
      auto stream = client.open_stream();

      if (!req.response_ch)
      {
        // This should never happen, as it should have been checked before getting here.
        throw std::runtime_error("response_ch is required!");
      }

      stream.on_headers(
        [&](const ::http::headers& h) {
          auto status = h.get(":status");
          mistral::trace::logger::instance().trace(mistral::trace::category::stream, "on_headers status='{}'", status);
          if (!status.empty()) {
            std::from_chars(status.data(), status.data() + status.size(), response_.status_code);
          }
        }
      );

      stream.on_data(
        [&](std::span<const std::byte> data)
        {
          auto* first = reinterpret_cast<const char*>(data.data());
          response_.body.append(first, first + data.size());
          mistral::trace::logger::instance().trace(mistral::trace::category::stream, "on_data body={}", response_.body);
          if (req.stream) {
            sse_parser_.parse(
              data,
              [&](http::sse_event evt) {
                if (evt.data == "[DONE]") {
                  req.response_ch->try_send({api::event_type::done, {}});
                } else if (!evt.data.empty()) {
                  req.response_ch->try_send({api::event_type::data, std::move(evt.data)});
                }
              }
            );
          }
        }
      );

      stream.on_end(
        [&]() {
          mistral::trace::logger::instance().trace(mistral::trace::category::stream, "on_end status_code='{}', req.stream={}", response_.status_code, req.stream);
          if (response_.status_code < 200 || response_.status_code >= 300)
          {
            req.response_ch->try_send({api::event_type::error,
              std::format("HTTP {}: {}", response_.status_code, response_.body)});
          }
          else
          {
            if (!req.stream)
            {
              req.response_ch->try_send({api::event_type::data, std::move(response_.body)});
              req.response_ch->try_send({api::event_type::done, {}});
            }
            else
            {
              // For successful streaming, SSE parser already sent [DONE]
            }
          }
          response_.done = true;
        }
      );

      stream.on_reset(
        [&](std::error_code ec) {
          mistral::trace::logger::instance().trace(mistral::trace::category::stream, "on_reset error_code='{}'", ec.message());
          req.response_ch->try_send({api::event_type::error,
            std::format("stream reset: {}", ec.message())});
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
    http::sse_parser sse_parser_;
    api::api_response response_;
    bool connected_ = false;

    std::shared_ptr<dripcore::channel<api::api_request>> request_ch_;
  };
} // namespace mistral::net

#endif // MISTRAL_HTTP_TASK_H