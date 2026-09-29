#ifndef MISTRAL_HTTP_COORDINATOR_TASK_H
#define MISTRAL_HTTP_COORDINATOR_TASK_H

#include "http/http_client.h"
#include "http/http_coordinator_event.h"
#include "http/http_request_task.h"
#include "http/http_read_task.h"
#include "http/http_write_task.h"
#include "http/tls_connection.h"
#include "api/types.h"
#include "trace/trace.h"

#include <dripcore/task.h>
#include <dripcore/channel.h>
#include <dripcore/task_handle.h>
#include <dripcore/cancelled.h>
#include <dripcore/task_control.h>
#include <dripcore/awaitable.h>

#include <http/headers.h>
#include <http/stream.h>
#include <http/sse.h>
#include <http/event.h>

#include <openssl/ssl.h>

#include <charconv>
#include <memory>
#include <string>
#include <variant>
#include <format>
#include <span>
#include <unordered_map>
#include <iostream>
#include <functional>

namespace mistral::net
{
  // Per-stream state held by the coordinator while a request is in flight.
  // The coordinator's dispatch_protocol_event() updates this state as
  // protocol events arrive from the engine.
  struct stream_response_state
  {
    std::shared_ptr<api::api_response> response;
    std::shared_ptr<http::sse_parser> sse_parser;
    std::shared_ptr<dripcore::channel<api::api_event>> response_ch;
    std::shared_ptr<dripcore::channel<bool>> completion_ch;
    bool req_stream = false;

    // Flow control: request body not yet fully accepted by the engine.
    // try_send_remaining() is invoked when a window-available event arrives.
    http::stream stream;
    std::vector<std::byte> pending_body;
    size_t send_offset = 0;
    bool end_stream_sent = false;

    void try_send_remaining()
    {
      while (send_offset < pending_body.size())
      {
        size_t remaining = pending_body.size() - send_offset;
        size_t sent = stream.send_data(
          std::span<const std::byte>(pending_body.data() + send_offset, remaining),
          false);
        send_offset += sent;
        if (sent == 0) break;  // window exhausted
      }
    }

    // Send END_STREAM if all body data has been accepted by the engine
    // and END_STREAM has not already been sent.
    void try_end_stream()
    {
      if (!end_stream_sent && send_offset >= pending_body.size()) {
        stream.send_end();
        end_stream_sent = true;
      }
    }

    // Called on stream_closed — send final response events to the client.
    void finalize()
    {
      mistral::trace::logger::instance().trace(
        mistral::trace::category::stream,
        "on_end status_code='{}', req.stream={}", response->status_code, req_stream);
      if (response->status_code < 200 || response->status_code >= 300)
      {
        response_ch->try_send({api::event_type::error,
          std::format("HTTP {}: {}", response->status_code, response->body),
          response->status_code});
      }
      else if (!req_stream)
      {
        response_ch->try_send({api::event_type::data, std::move(response->body)});
      }
      // Always send done on stream close. For streaming requests the SSE
      // parser may have already sent done on [DONE], but some providers
      // close the stream without a [DONE] sentinel, so this guarantees
      // the consumer's event loop terminates.
      response->done = true;
      response_ch->try_send({api::event_type::done, {}});
      if (completion_ch) completion_ch->try_send(true);
    }

    // Called on stream_reset — send error and signal completion.
    void fail(const std::string& msg)
    {
      response_ch->try_send({api::event_type::error, msg});
      response->done = true;
      if (completion_ch) completion_ch->try_send(true);
    }
  };

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

  protected:
    dripcore::awaitable<> body() override
    {
      // Spawn the request receiver task first
      spawn_local<http_request_task>(request_ch_, event_ch_);

      // Main event loop
      while (true) {
        auto evt = co_await event_ch_->recv(this);
        if (!evt) break;  // Channel closed

        switch (evt->type) {
          case coordinator_event_type::new_request:
            co_await handle_new_request(*evt->request, evt->completion_ch);
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
            co_return;
        }
      }

      co_return;
    }

    dripcore::awaitable<> teardown() override
    {
      handle_shutdown();
      co_return;
    }

  private:
    dripcore::awaitable<void> handle_new_request(const api::api_request& req, std::shared_ptr<dripcore::channel<bool>> completion_ch)
    {
      auto response_ch = req.response_ch;

      if (!response_ch) {
        std::cerr << "[http_coordinator] response_ch is required!" << std::endl;
        if (completion_ch) {
          completion_ch->try_send(true);
        }
        co_return;
      }

      // If not connected, establish connection
      if (!client_.connected()) {
        try {
          std::cerr << "[http_coordinator] " << host_ << " connecting..." << std::endl;
          client_.connect(host_, port_);

          // Advertise a larger receive window (1 MB) to reduce WINDOW_UPDATE
          // frequency and prevent stalls during large response reception.
          // Must be called before open_stream() so SETTINGS includes the value.
          client_.set_initial_window_size(1048576);

          // Register output readiness callback — signals the write task
          // when the engine has output ready to send.
          client_.on_output_ready([this]() {
            if (output_ready_ch_) {
              output_ready_ch_->try_send(true);
            }
          });

          // Spawn the I/O tasks only after connection is established.
          // Pass a direct event handler to the reader — protocol events
          // are dispatched inline, avoiding a channel hop. This is safe
          // because all tasks run on the same single-threaded executor.
          reader_task_ = spawn_local<http_read_task>(
            std::ref(client_), event_ch_,
            [this](http::event& ev) { dispatch_protocol_event(ev); }
          );
          writer_task_ = spawn_local<http_write_task>(std::ref(client_), event_ch_, output_ready_ch_);
        }
        catch (const std::exception& e) {
          std::cerr << "[http_coordinator] connection failed: " << e.what() << std::endl;
          response_ch->try_send({api::event_type::error, e.what()});
          response_ch->close();
          if (completion_ch) {
            completion_ch->try_send(true);
          }
          co_return;
        }
      }

      // Open a new stream for this request
      auto stream = client_.open_stream();

      // Create per-stream state and register it so protocol events
      // dispatched by the read task are routed to this stream.
      auto state = std::make_shared<stream_response_state>();
      state->response = std::make_shared<api::api_response>();
      state->sse_parser = std::make_shared<http::sse_parser>();
      state->response_ch = response_ch;
      state->completion_ch = completion_ch;
      state->req_stream = req.stream;
      state->stream = stream;

      stream_registry_[stream.id()] = state;

      // Send the request (eager — no suspension for flow control).
      // If the engine throws during send_headers or send_data, fail the
      // stream so completion_ch is signaled and the request pipeline does
      // not deadlock waiting for a response that will never arrive.
      try {
        send_request_body(stream, req, state);
      }
      catch (const std::exception& e) {
        state->fail(std::format("send error: {}", e.what()));
        stream_registry_.erase(stream.id());
      }
    }

    void dispatch_protocol_event(http::event& evt)
    {
      // Connection-level events are forwarded as coordinator events
      // so they are handled in the coordinator's event loop, not inline.
      switch (evt.type)
      {
      case http::event_type::goaway:
      {
        auto& goaway = std::get<http::goaway_info>(evt.payload);
        event_ch_->try_send({
          coordinator_event_type::goaway_received, {},
          http::make_error_code(goaway.ec), goaway.last_stream_id
        });
        return;
      }

      case http::event_type::connection_error:
      {
        auto ec = std::get<std::error_code>(evt.payload);
        event_ch_->try_send({coordinator_event_type::connection_error, {}, ec});
        return;
      }

      default:
        break;
      }

      // Per-stream events — look up the stream in the registry.
      auto it = stream_registry_.find(evt.stream_id);
      if (it == stream_registry_.end()) {
        return;
      }
      auto& state = it->second;

      switch (evt.type)
      {
      case http::event_type::headers:
      {
        auto& h = std::get<http::headers>(evt.payload);
        auto status = h.get(":status");
        mistral::trace::logger::instance().trace(
          mistral::trace::category::stream, "on_headers status='{}'", status);
        if (!status.empty()) {
          std::from_chars(status.data(), status.data() + status.size(),
            state->response->status_code);
        }
        break;
      }

      case http::event_type::data:
      {
        // If the consumer closed the response channel (cancelled),
        // fail the stream immediately so completion_ch is signaled
        // and http_request_task unblocks instead of waiting for the
        // full server response.
        if (state->response_ch->is_closed()) {
          state->fail("cancelled");
          stream_registry_.erase(it);
          return;
        }

        auto& data = std::get<std::vector<std::byte>>(evt.payload);
        auto* first = reinterpret_cast<const char*>(data.data());
        state->response->body.append(first, first + data.size());
        mistral::trace::logger::instance().trace(
          mistral::trace::category::stream, "on_data body={}",
          state->response->body);
        if (state->req_stream) {
          std::span<const std::byte> data_span(data.data(), data.size());
          state->sse_parser->parse(
            data_span,
            [&state](http::sse_event sse_evt) {
              if (sse_evt.data == "[DONE]") {
                state->response_ch->try_send({api::event_type::done, {}});
              } else if (!sse_evt.data.empty()) {
                state->response_ch->try_send({api::event_type::data, std::move(sse_evt.data)});
              }
            }
          );
        }
        break;
      }

      case http::event_type::stream_closed:
        state->finalize();
        stream_registry_.erase(it);
        break;

      case http::event_type::stream_reset:
      {
        auto ec = std::get<std::error_code>(evt.payload);
        mistral::trace::logger::instance().trace(
          mistral::trace::category::stream, "on_reset error_code='{}'", ec.message());
        state->fail(std::format("stream reset: {}", ec.message()));
        stream_registry_.erase(it);
        break;
      }

      case http::event_type::stream_window_available:
        state->try_send_remaining();
        state->try_end_stream();
        break;

      default:
        break;
      }
    }

    void send_request_body(
      http::stream& stream,
      const api::api_request& req,
      std::shared_ptr<stream_response_state> state
    )
    {
      ::http::headers hdrs{
        {"content-type", "application/json"},
        {"authorization", "Bearer " + api_key_},
        {"accept", req.stream ? "text/event-stream" : "application/json"},
        {"user-agent", "mistral-cli/1.0"},
      };

      stream.send_headers(req.method, req.path, host_, hdrs, false);

      auto body_bytes = std::as_bytes(std::span{req.body.data(), req.body.size()});

      // Store pending body in stream state for flow-controlled sending.
      // try_send_remaining() is called on stream_window_available events.
      state->pending_body.assign(body_bytes.begin(), body_bytes.end());
      state->send_offset = 0;
      state->try_send_remaining();
      // If the body is empty (or fully accepted on first try), send
      // END_STREAM now so the server knows the request is complete.
      state->try_end_stream();
    }

    void fail_all_streams(const std::string& reason)
    {
      for (auto& [id, state] : stream_registry_) {
        state->fail(reason);
      }
      stream_registry_.clear();
    }

    void handle_connection_closed()
    {
      std::cerr << "[http_coordinator] Connection closed" << std::endl;
      fail_all_streams("connection closed");
      // Reset client to allow reconnection
      client_.reset();
      // Stop reader/writer tasks.
      reader_task_.cancel();
      writer_task_.cancel();
    }

    void handle_connection_error(std::error_code ec)
    {
      std::cerr << "[http_coordinator] Connection error: " << ec.message() << std::endl;
      fail_all_streams(std::format("connection error: {}", ec.message()));
      // Reset client to allow reconnection
      client_.reset();
      // Stop reader/writer tasks.
      reader_task_.cancel();
      writer_task_.cancel();
    }

    void handle_goaway(uint32_t last_stream_id, std::error_code ec)
    {
      std::cerr << "[http_coordinator] GOAWAY received, last_stream_id=" << last_stream_id
                << ", error=" << ec.message() << " (active_streams=" << client_.active_streams() << ")" << std::endl;
      fail_all_streams(std::format("goaway: {}", ec.message()));
      client_.shutdown(); // Same reasoning as for coordinator_event_type::shutdown
    }

    void handle_shutdown()
    {
      fail_all_streams("shutdown");
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
    std::unordered_map<uint32_t, std::shared_ptr<stream_response_state>> stream_registry_;
  };
} // namespace mistral::net

#endif // MISTRAL_HTTP_COORDINATOR_TASK_H