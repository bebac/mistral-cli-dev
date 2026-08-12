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

  public:
    dripcore::task_handle execute()
    {
      auto& client = client_.get();

      try {
        while (client.connected()) {
          auto out = client.output_begin();

          // Empty output buffer means nothing to send
          if (out.empty()) {
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
            client.output_end(n);
          }
          else {
            int err = client.get_error(n);

            if (err == SSL_ERROR_WANT_WRITE) {
              co_await writable(client);
            }
            else if (err == SSL_ERROR_WANT_READ) {
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
      }
      catch (const dripcore::cancelled&) {
        // Task was cancelled, clean up gracefully
        std::cerr << "[http_write] task cancelled, shutting down" << std::endl;
        event_ch_->try_send({coordinator_event_type::connection_closed});
      }
      catch (const std::exception& e) {
        std::cerr << "[http_write] exception: " << e.what() << std::endl;
        event_ch_->try_send({
          coordinator_event_type::connection_error,
          {},
          std::make_error_code(std::errc::io_error)
        });
      }

      co_return;
    }

  private:
    std::reference_wrapper<http_client> client_;
    std::shared_ptr<dripcore::channel<coordinator_event>> event_ch_;
    std::shared_ptr<dripcore::channel<bool>> output_ready_ch_;
  };
} // namespace mistral::net

#endif // MISTRAL_HTTP_WRITE_TASK_H
