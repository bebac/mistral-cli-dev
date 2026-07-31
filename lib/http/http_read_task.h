#ifndef MISTRAL_HTTP_READ_TASK_H
#define MISTRAL_HTTP_READ_TASK_H

#include "http/http_client.h"
#include "http/http_coordinator_event.h"
#include "api/mistral_types.h"
#include "trace/trace.h"

#include <dripcore/task.h>
#include <dripcore/channel.h>
#include <dripcore/awaitable.h>

#include <openssl/err.h>

#include <memory>
#include <string>

namespace mistral::net
{
  // Task that reads from the socket and feeds data to the HTTP client
  class http_read_task : public dripcore::task
  {
  public:
    http_read_task(
      std::reference_wrapper<http_client> client,
      std::shared_ptr<dripcore::channel<coordinator_event>> event_ch
    )
      : client_(client)
      , event_ch_(std::move(event_ch))
    {
    }

  public:
    dripcore::task_handle execute()
    {
      auto& client = client_.get();

      while (client.connected()) {
        auto in = client.input_begin();

        if (in.empty()) {
          // No input buffer available, wait for readable
          co_await readable(client);
          continue;
        }

        int n = client.read(in);

        if (n > 0) {
          mistral::trace::logger::instance().trace(mistral::trace::category::data, "read {}", in.subspan(0, n));
          client.input_end(n);  // Processes HTTP/2 frames, fires callbacks
          co_await yield();
        }
        else {
          int err = client.get_error(n);

          if (err == SSL_ERROR_WANT_READ) {
            // Only wait on fd if SSL has no pending data
            if (client.no_data_pending()) {
              co_await readable(client);
            }
          }
          else if (err == SSL_ERROR_WANT_WRITE) {
            co_await writable(client);
          }
          else if (err == SSL_ERROR_ZERO_RETURN) {
            // Connection closed gracefully by peer
            event_ch_->try_send({coordinator_event_type::connection_closed});
            break;
          }
          else if (err == SSL_ERROR_SYSCALL) {
            if (errno == 0 || errno == ECONNRESET) {
              event_ch_->try_send({coordinator_event_type::connection_closed});
              break;
            }
            else {
              std::cerr << "[http_read] connection error(1) fd=" << client.platform_handle() << std::endl;
              event_ch_->try_send({
                coordinator_event_type::connection_error,
                {},
                std::make_error_code(static_cast<std::errc>(errno))});
              break;
            }
          }
          else if (err == SSL_ERROR_SSL) {
            auto [msg, ec] = tls_connection::get_ssl_error_details();
            std::cerr << "[http_read] " << msg << " fd=" << client.platform_handle() << std::endl;
            event_ch_->try_send({
              coordinator_event_type::connection_error,
              {},
              ec});
            break;
          }
          else {
            std::cerr << "[http_read] connection error(2) fd=" << client.platform_handle() << std::endl;
            // For any other SSL error, use errno as fallback
            event_ch_->try_send({
              coordinator_event_type::connection_error,
              {},
              std::make_error_code(static_cast<std::errc>(errno))});
            break;
          }
        }
      }

      co_return;
    }

  private:
    std::reference_wrapper<http_client> client_;
    std::shared_ptr<dripcore::channel<coordinator_event>> event_ch_;
  };

} // namespace mistral::net

#endif // MISTRAL_HTTP_READ_TASK_H
