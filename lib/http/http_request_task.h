#ifndef MISTRAL_HTTP_REQUEST_TASK_H
#define MISTRAL_HTTP_REQUEST_TASK_H

#include "http/http_coordinator_event.h"
#include "api/mistral_types.h"

#include <dripcore/task.h>
#include <dripcore/channel.h>
#include <dripcore/cancelled.h>

#include <memory>

namespace mistral::net
{
  // Task that receives API requests and forwards them to the coordinator
  class http_request_task : public dripcore::task
  {
  public:
    http_request_task(
      std::shared_ptr<dripcore::channel<api::api_request>> request_ch,
      std::shared_ptr<dripcore::channel<coordinator_event>> event_ch
    )
      : request_ch_(std::move(request_ch))
      , event_ch_(std::move(event_ch))
    {
    }

  protected:
    dripcore::awaitable<> body() override
    {
      while (true) {
        auto req = co_await request_ch_->recv(this);

        // No request means we are shutting down
        if (!req) {
          event_ch_->try_send({coordinator_event_type::shutdown});
          break;
        }

        // Create a completion channel for this request
        auto completion_ch = std::make_shared<dripcore::channel<bool>>(1);

        // Forward the request to the coordinator with completion notification
        event_ch_->try_send({coordinator_event_type::new_request, std::move(req), {}, 0, completion_ch});

        // Wait for the request to complete before accepting the next one
        co_await completion_ch->recv(this);
      }

      co_return;
    }

    dripcore::awaitable<> teardown() override
    {
      // On cancellation or error, signal shutdown to the coordinator.
      event_ch_->try_send({coordinator_event_type::shutdown});
      co_return;
    }

  private:
    std::shared_ptr<dripcore::channel<api::api_request>> request_ch_;
    std::shared_ptr<dripcore::channel<coordinator_event>> event_ch_;
  };
} // namespace mistral::net

#endif // MISTRAL_HTTP_REQUEST_TASK_H
