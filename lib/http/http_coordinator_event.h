#ifndef MISTRAL_HTTP_COORDINATOR_EVENT_H
#define MISTRAL_HTTP_COORDINATOR_EVENT_H

#include "api/types.h"

#include <dripcore/channel.h>

#include <memory>
#include <system_error>

namespace mistral::net
{
  // Events for the coordinator task
  enum class coordinator_event_type
  {
    new_request,
    connection_closed,
    connection_error,
    goaway_received,
    shutdown
  };

  struct coordinator_event
  {
    coordinator_event_type type;
    std::optional<api::api_request> request;
    std::error_code error;
    uint32_t last_stream_id = 0;
    std::shared_ptr<dripcore::channel<bool>> completion_ch;
  };
} // namespace mistral::net

#endif // MISTRAL_HTTP_COORDINATOR_EVENT_H
