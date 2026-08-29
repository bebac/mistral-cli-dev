#ifndef MISTRAL_HTTP_WRITE_TASK_H
#define MISTRAL_HTTP_WRITE_TASK_H

#include "http/http_client.h"
#include "api/mistral_types.h"

#include <dripcore/task.h>
#include <dripcore/channel.h>
#include <dripcore/awaitable.h>
#include <dripcore/cancelled.h>

#include <openssl/err.h>

#include <memory>
#include <string>
#include <iostream>
#include <functional>
#include <cerrno>
#include <cstring>
#include <cstddef>

namespace mistral::net
{
  // Task that writes to the socket from the HTTP client's output buffer
  class http_write_task : public dripcore::task
  {
  public:
    http_write_task(
      std::reference_wrapper<http_client> client,
      std::shared_ptr<dripcore::channel<coordinator_event>> event_ch,
      std::shared_ptr<dripcore::channel<bool>> output_ready_ch
    )
      : client_(client)
      , event_ch_(std::move(event_ch))
      , output_ready_ch_(std::move(output_ready_ch))
    {
    }

  protected:
    dripcore::awaitable<> body() override
    {
      auto& client = client_.get();

      // When SSL_write returns WANT_WRITE or WANT_READ, OpenSSL requires
      // the same buffer contents on retry. With SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER
      // enabled (set on the SSL_CTX), the pointer may differ but contents must
      // match. Data is only ever appended to the engine's pending_out_ vector —
      // never modified in place — and only removed from the front via
      // output_end() (which we don't call on WANT_WRITE). So on retry we can
      // simply re-fetch output_begin() and subspan to the remembered length,
      // avoiding a buffer copy.
      size_t retry_len = 0;

      while (client.connected()) {
        auto out = client.output_begin();

        if (retry_len > 0 && retry_len < out.size()) {
          out = out.first(retry_len);
        }

        // Empty output buffer means nothing to send
        if (out.empty()) {
          retry_len = 0;
          if (auto res = co_await output_ready_ch_->recv(this)) {
            continue;
          }
          else {
            break;
          }
        }

        if (!client.connected()) {
          std::cerr << "[http_write] connection closed" << std::endl;
          break;
        }

        int n = client.write(out);

        if (n > 0) {
          retry_len = 0;
          client.output_end(n);
        }
        else {
          int err = client.get_error(n);

          if (err == SSL_ERROR_WANT_WRITE) {
            retry_len = out.size();
            co_await writable(client);
          }
          else if (err == SSL_ERROR_WANT_READ) {
            retry_len = out.size();
            co_await readable(client);
          }
          else if (err == SSL_ERROR_SYSCALL) {
            // Connection closed or reset - not a fatal error, allows retry
            if (errno == 0 || errno == EPIPE || errno == ECONNRESET) {
              std::cerr << "[http_write_task] SSL_write: connection closed error: " << errno
                        << "(" << strerror(errno) << ")" << std::endl;
              event_ch_->try_send({coordinator_event_type::connection_closed});
              break;
            }
            else {
              std::cerr << "[http_write] send connection error(1)" << std::endl;
              event_ch_->try_send({
                coordinator_event_type::connection_error,
                {},
                std::make_error_code(static_cast<std::errc>(errno))
              });
              break;
            }
          }
          else if (err == SSL_ERROR_SSL) {
            if (client.is_shutting_down()) {
              event_ch_->try_send({coordinator_event_type::connection_closed});
              break;
            }
            auto [msg, ec] = tls_connection::get_ssl_error_details();
            std::cerr << "[http_write] " << msg << std::endl;
            event_ch_->try_send({
              coordinator_event_type::connection_error,
              {},
              ec
            });
            break;
          }
          else {
            std::cerr << "[http_write] send connection error(2)" << std::endl;
            // For any other SSL error, use errno as fallback
            event_ch_->try_send({
              coordinator_event_type::connection_error,
              {},
              std::make_error_code(static_cast<std::errc>(errno))
            });
            break;
          }
        }
      }

      co_return;
    }

    dripcore::awaitable<> teardown() override
    {
      // On cancellation or error, notify the coordinator that the
      // connection is gone so it can fail pending streams.
      event_ch_->try_send({coordinator_event_type::connection_closed});
      co_return;
    }

  private:
    std::reference_wrapper<http_client> client_;
    std::shared_ptr<dripcore::channel<coordinator_event>> event_ch_;
    std::shared_ptr<dripcore::channel<bool>> output_ready_ch_;
  };
} // namespace mistral::net

#endif // MISTRAL_HTTP_WRITE_TASK_H
