#ifndef MISTRAL_HTTP_COORDINATOR_TASK_H
#define MISTRAL_HTTP_COORDINATOR_TASK_H

#include "http/http_client.h"
#include "http/http_coordinator_event.h"
#include "http/http_request_task.h"
#include "http/http_read_task.h"
#include "http/http_write_task.h"
#include "http/tls_connection.h"
#include "api/mistral_types.h"
#include "trace/trace.h"

#include <dripcore/task.h>
#include <dripcore/channel.h>
#include <dripcore/task_handle.h>
#include <dripcore/cancelled.h>
#include <dripcore/task_control.h>

#include <http/headers.h>
#include <http/stream.h>
#include <http/sse.h>

#include <openssl/ssl.h>

#include <memory>
#include <string>
#include <variant>
#include <vector>
#include <format>
#include <span>
#include <iostream>
#include <functional>

namespace mistral::net
{
  // Main coordinator task that owns the HTTP client and manages requests
  class http_coordinator_task : public dripcore::task
  {
  public:
    http_coordinator_task(
      std::string host,
      int port,
      std::string api_key,
      std::shared_ptr<dripcore::channel<api::api_request>> request_ch
    )
      : host_(std::move(host))
      , port_(port)
      , api_key_(std::move(api_key))
      , request_ch_(std::move(request_ch))
      , event_ch_(std::make_shared<dripcore::channel<coordinator_event>>(32))
      , output_ready_ch_(std::make_shared<dripcore::channel<bool>>(1))
    {
    }

  public:
    dripcore::task_handle execute()
    {
      // Spawn the request receiver task first
      spawn<http_request_task>(request_ch_, event_ch_);

      // Main event loop
      try {
        while (true) {
          auto evt = co_await event_ch_->recv(this);
          if (!evt) break;  // Channel closed

          switch (evt->type) {
            case coordinator_event_type::new_request:
              handle_new_request(*evt->request, evt->completion_ch);
              break;

            case coordinator_event_type::connection_closed:
              handle_connection_closed();
              break;

            case coordinator_event_type::connection_error:
              handle_connection_error(evt->error);
              break;

            case coordinator_event_type::goaway_received:
              handle_goaway(evt->last_stream_id, evt->error);
              break;

            case coordinator_event_type::shutdown:
              handle_shutdown();
              co_return;
          }
        }
      }
      catch (const dripcore::cancelled&) {
        // Task was cancelled, clean up gracefully
        std::cerr << "[http_coordinator] task cancelled, shutting down" << std::endl;
        handle_shutdown();
      }
      catch (const std::exception& e) {
        std::cerr << "[http_coordinator] exception: " << e.what() << std::endl;
        handle_shutdown();
      }

      co_return;
    }

  private:
    void handle_new_request(const api::api_request& req, std::shared_ptr<dripcore::channel<bool>> completion_ch)
    {
      auto response_ch = req.response_ch;

      // Create per-request state
      auto response = std::make_shared<api::api_response>();
      auto sse_parser = std::make_shared<http::sse_parser>();

      if (!response_ch) {
        std::cerr << "[http_coordinator] response_ch is required!" << std::endl;
        return;
      }

      // If not connected, establish connection
      if (!client_.connected()) {
        try {
          std::cerr << "[http_coordinator] " << host_ << " connecting..." << std::endl;
          client_.connect(host_, port_);

          // Register connection-level callbacks
          client_.on_goaway([this](uint32_t last_stream_id, std::error_code ec) {
            event_ch_->try_send({coordinator_event_type::goaway_received, {}, ec, last_stream_id});
          });

          client_.on_connection_error([this](std::error_code ec) {
            event_ch_->try_send({coordinator_event_type::connection_error, {}, ec, 0});
          });

          client_.on_output_ready([this]() {
            if (output_ready_ch_) {
              output_ready_ch_->try_send(true);
            }
          });

          // Spawn the I/O tasks only after connection is established
          reader_task_ = spawn<http_read_task>(std::ref(client_), event_ch_);
          writer_task_ = spawn<http_write_task>(std::ref(client_), event_ch_, output_ready_ch_);
        }
        catch (const std::exception& e) {
          std::cerr << "[http_coordinator] connection failed: " << e.what() << std::endl;
          response_ch->try_send({api::event_type::error, e.what()});
          response_ch->close();
          if (completion_ch) {
            completion_ch->try_send(true);
          }
          return;
        }
      }

      // Open a new stream for this request
      auto stream = client_.open_stream();

      // Set up stream callbacks - these directly send to response_ch
      setup_stream_callbacks(stream, req, response_ch, completion_ch, response, sse_parser);

      // Initialize and send the request
      init_request(stream, req);
    }

    void setup_stream_callbacks(
      http::stream& stream,
      const api::api_request& req,
      std::shared_ptr<dripcore::channel<api::api_event>> response_ch,
      std::shared_ptr<dripcore::channel<bool>> completion_ch,
      std::shared_ptr<api::api_response> response,
      std::shared_ptr<http::sse_parser> sse_parser
    )
    {
      stream.on_headers(
        [response_ch, response](const ::http::headers& h) {
          auto status = h.get(":status");
          mistral::trace::logger::instance().trace(mistral::trace::category::stream, "on_headers status='{}'", status);
          if (!status.empty()) {
            std::from_chars(status.data(), status.data() + status.size(), response->status_code);
          }
        }
      );

      stream.on_data(
        [response_ch, req_stream = req.stream, response, sse_parser](std::span<const std::byte> data) {
          auto* first = reinterpret_cast<const char*>(data.data());
          response->body.append(first, first + data.size());
          mistral::trace::logger::instance().trace(mistral::trace::category::stream, "on_data body={}", response->body);
          if (req_stream) {
            sse_parser->parse(
              data,
              [&](http::sse_event evt) {
                if (evt.data == "[DONE]") {
                  response_ch->try_send({api::event_type::done, {}});
                } else if (!evt.data.empty()) {
                  response_ch->try_send({api::event_type::data, std::move(evt.data)});
                }
              }
            );
          }
        }
      );

      stream.on_end(
        [response_ch, req_stream = req.stream, completion_ch, response]() {
          mistral::trace::logger::instance().trace(mistral::trace::category::stream, "on_end status_code='{}', req.stream={}", response->status_code, req_stream);
          if (response->status_code < 200 || response->status_code >= 300)
          {
            response_ch->try_send({api::event_type::error,
              std::format("HTTP {}: {}", response->status_code, response->body)});
          }
          else
          {
            if (!req_stream)
            {
              response_ch->try_send({api::event_type::data, std::move(response->body)});
              response_ch->try_send({api::event_type::done, {}});
            }
            else
            {
              // For successful streaming, SSE parser already sent [DONE]
            }
          }
          response->done = true;
          // Notify request task that this request is complete
          if (completion_ch) {
            completion_ch->try_send(true);
          }
        }
      );

      stream.on_reset(
        [response_ch, completion_ch, response](std::error_code ec) {
          mistral::trace::logger::instance().trace(mistral::trace::category::stream, "on_reset error_code='{}'", ec.message());
          response_ch->try_send({api::event_type::error,
            std::format("stream reset: {}", ec.message())});
          response->done = true;
          // Notify request task that this request is complete
          if (completion_ch) {
            completion_ch->try_send(true);
          }
        }
      );
    }

    void init_request(http::stream& stream, const api::api_request& req)
    {
      ::http::headers hdrs{
        {"content-type", "application/json"},
        {"authorization", "Bearer " + api_key_},
        {"accept", req.stream ? "text/event-stream" : "application/json"},
        {"user-agent", "mistral-cli/1.0"},
      };

      stream.send_headers(req.method, req.path, host_, hdrs, false);

      auto body_bytes = std::as_bytes(std::span{req.body.data(), req.body.size()});
      stream.send_data(body_bytes, true);
    }

    void handle_connection_closed()
    {
      std::cerr << "[http_coordinator] Connection closed" << std::endl;
      // Reset client to allow reconnection
      client_.reset();
    }

    void handle_connection_error(std::error_code ec)
    {
      std::cerr << "[http_coordinator] Connection error: " << ec.message() << std::endl;
      // Reset client to allow reconnection
      client_.reset();
    }

    void handle_goaway(uint32_t last_stream_id, std::error_code ec)
    {
      std::cerr << "[http_coordinator] GOAWAY received, last_stream_id=" << last_stream_id
                << ", error=" << ec.message() << " (active_streams=" << client_.active_streams() << ")" << std::endl;
      client_.shutdown(); // Same reasinong as for coordinator_event_type::shutdown
    }

    void handle_shutdown()
    {
      reader_task_.cancel();
      writer_task_.cancel();
    }

  private:
    std::string host_;
    int port_;
    std::string api_key_;

    http_client client_;

    dripcore::task_control reader_task_;
    dripcore::task_control writer_task_;

    std::shared_ptr<dripcore::channel<api::api_request>> request_ch_;
    std::shared_ptr<dripcore::channel<coordinator_event>> event_ch_;
    std::shared_ptr<dripcore::channel<bool>> output_ready_ch_;
  };
} // namespace mistral::net

#endif // MISTRAL_HTTP_COORDINATOR_TASK_H
