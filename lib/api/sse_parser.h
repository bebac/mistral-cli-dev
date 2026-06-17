#ifndef MISTRAL_SSE_PARSER_H
#define MISTRAL_SSE_PARSER_H

#include <string>
#include <string_view>
#include <vector>

namespace mistral::api
{
  // Stateful parser for Server-Sent Events streams.
  // Feed chunks of data as they arrive; get back complete event payloads.
  class sse_parser
  {
  public:
    struct event
    {
      enum class type { data, done };
      type kind;
      std::string payload;  // The JSON string (for data events)
    };

    // Feed a chunk of raw SSE data. Returns any complete events found.
    std::vector<event> feed(std::string_view chunk)
    {
      buffer_.append(chunk);
      std::vector<event> events;

      while (true)
      {
        auto pos = buffer_.find('\n');
        if (pos == std::string::npos) break;

        std::string line = buffer_.substr(0, pos);
        buffer_.erase(0, pos + 1);

        // Strip trailing \r if present.
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }

        // Empty line = end of event block.
        if (line.empty()) {
          if (!current_data_.empty()) {
            if (current_data_ == "[DONE]") {
              events.push_back({event::type::done, {}});
            } else {
              events.push_back({event::type::data, std::move(current_data_)});
            }
            current_data_.clear();
          }
          continue;
        }

        // Parse "data: ..." lines.
        if (line.starts_with("data: ")) {
          auto payload = line.substr(6);
          current_data_.append(payload);
        }
        // Ignore other SSE fields (event:, id:, retry:) for now.
      }

      return events;
    }

    void reset()
    {
      buffer_.clear();
      current_data_.clear();
    }

  private:
    std::string buffer_;
    std::string current_data_;
  };

} // namespace mistral::api

#endif // MISTRAL_SSE_PARSER_H
