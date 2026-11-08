#include "knowledge/mistral_embedder.h"

#include <dripcore/executor.h>
#include <dripcore/sleep_awaiter.h>

#include <chrono>
#include <cstdint>
#include <format>
#include <iterator>
#include <utility>

namespace mistral::knowledge
{
  namespace
  {
    // How often embed_batch() re-checks the response channel while
    // waiting. Small enough to be invisible next to a network round
    // trip, large enough that a waiting request is not a busy loop.
    constexpr auto k_poll_interval = std::chrono::milliseconds(10);

    // Attempts per request. A dropped connection is a normal event on a
    // long-lived HTTP/2 connection — servers recycle them — and the
    // coordinator reconnects on the next request, so one retry is
    // usually enough. Four covers a reconnect that itself fails once.
    constexpr int k_max_attempts = 4;

    // Linear backoff between attempts. Without it, several requests that
    // failed together would all reconnect at the same instant.
    constexpr int k_retry_backoff_ms = 250;

    // Transport failures worth retrying. Mirrors
    // agent_task_base::is_retryable_error, which already retries these
    // for chat requests; the embedder was the one path without it.
    //
    // "closed without a response" is deliberately absent: the coordinator
    // closes a response channel without sending an event only when it is
    // being torn down, where a retry cannot succeed. A dropped connection
    // arrives as an error event instead, and is covered here.
    bool is_retryable_transport_error(const std::string& error)
    {
      return error.find("connection closed") != std::string::npos ||
             error.find("connection error") != std::string::npos ||
             error.find("stream reset") != std::string::npos ||
             error.find("send error") != std::string::npos;
    }
  } // namespace

  std::string build_embeddings_request(
    const std::string& model,
    const std::vector<std::string>& inputs,
    int output_dimension,
    const std::string& output_dtype
  )
  {
    api::json body = {
      {"model", model},
      {"input", inputs},
    };
    if (output_dimension > 0)
    {
      body["output_dimension"] = output_dimension;
    }
    if (!output_dtype.empty() && output_dtype != "float")
    {
      body["output_dtype"] = output_dtype;
    }
    return body.dump();
  }

  bool parse_embeddings_response(
    const std::string& body,
    std::size_t expected_count,
    std::vector<std::vector<float>>& out,
    std::string& error
  )
  {
    out.clear();

    api::json j;
    try
    {
      j = api::json::parse(body);
    }
    catch (const std::exception& e)
    {
      error = std::format("embeddings response is not valid JSON: {}", e.what());
      return false;
    }

    if (!j.contains("data") || !j["data"].is_array())
    {
      // The endpoint reports failures as
      // {"object":"error","message":"...","type":"..."}.
      if (j.contains("message") && j["message"].is_string())
      {
        error = j["message"].get<std::string>();
      }
      else
      {
        error = "embeddings response has no 'data' array";
      }
      return false;
    }

    const auto& data = j["data"];
    if (data.size() != expected_count)
    {
      error = std::format("embeddings response has {} vectors, expected {}", data.size(), expected_count);
      return false;
    }

    // data[].index is authoritative: the array order is not guaranteed.
    out.resize(expected_count);
    std::vector<bool> seen(expected_count, false);

    for (const auto& item : data)
    {
      if (!item.contains("index") || !item["index"].is_number_integer())
      {
        error = "embeddings response entry has no integer 'index'";
        out.clear();
        return false;
      }

      const auto raw_index = item["index"].get<std::int64_t>();
      if (raw_index < 0 || static_cast<std::size_t>(raw_index) >= expected_count)
      {
        error = std::format("embeddings response index {} is out of range", raw_index);
        out.clear();
        return false;
      }
      const auto index = static_cast<std::size_t>(raw_index);

      if (seen[index])
      {
        error = std::format("embeddings response repeats index {}", index);
        out.clear();
        return false;
      }

      if (!item.contains("embedding") || !item["embedding"].is_array())
      {
        error = std::format("embeddings response entry {} has no 'embedding' array", index);
        out.clear();
        return false;
      }

      const auto& embedding = item["embedding"];
      std::vector<float>& vec = out[index];
      vec.reserve(embedding.size());
      for (const auto& value : embedding)
      {
        if (!value.is_number())
        {
          error = std::format("embeddings response entry {} has a non-numeric component", index);
          out.clear();
          return false;
        }
        // int8 responses arrive as JSON integers; they widen to float
        // here. Storing them as int8 needs an int8 vec0 column, which is
        // a later optimisation.
        vec.push_back(value.get<float>());
      }

      if (vec.empty())
      {
        error = std::format("embeddings response entry {} is empty", index);
        out.clear();
        return false;
      }
      seen[index] = true;
    }

    // Every vector must share one dimension: the vec0 column is fixed.
    for (std::size_t i = 1; i < out.size(); ++i)
    {
      if (out[i].size() != out[0].size())
      {
        error = std::format(
          "embeddings response mixes dimensions: entry 0 has {}, entry {} has {}", out[0].size(), i, out[i].size()
        );
        out.clear();
        return false;
      }
    }

    return true;
  }

  mistral_embedder::mistral_embedder(
    std::shared_ptr<dripcore::channel<api::api_request>> request_ch,
    std::string model,
    std::string api_version,
    int output_dimension,
    std::string output_dtype,
    int batch_size,
    int timeout_ms,
    int max_batch_chars
  )
    : request_ch_(std::move(request_ch)), model_(std::move(model)), api_version_(std::move(api_version)),
      output_dimension_(output_dimension > 0 ? output_dimension : 1536), output_dtype_(std::move(output_dtype)),
      batch_size_(batch_size), timeout_(timeout_ms > 0 ? timeout_ms : 60000),
      max_batch_chars_(max_batch_chars > 0 ? max_batch_chars : 32000)
  {
  }

  std::string mistral_embedder::identity() const
  {
    return std::format("mistral/{}/{}/{}", model_, output_dimension_, output_dtype_.empty() ? "float" : output_dtype_);
  }

  dripcore::awaitable<std::vector<std::vector<float>>> mistral_embedder::embed_batch(
    std::vector<std::string> inputs,
    std::string& error
  )
  {
    std::vector<std::vector<float>> result;

    if (inputs.empty())
    {
      error = "embed_batch called with no inputs";
      co_return result;
    }
    if (static_cast<int>(inputs.size()) > k_max_inputs)
    {
      error = std::format(
        "embed_batch called with {} inputs; the embeddings endpoint accepts at most {}",
        inputs.size(),
        k_max_inputs
      );
      co_return result;
    }
    if (!request_ch_)
    {
      error = "no transport channel for the embeddings endpoint";
      co_return result;
    }

    // The endpoint limits both per-input tokens and total tokens per
    // request. batch_size bounds the count; max_batch_chars_ bounds the
    // total, because 32 full-size chunks trip "Too many tokens overall".
    std::size_t offset = 0;
    while (offset < inputs.size())
    {
      std::size_t count = 0;
      std::size_t chars = 0;
      while (offset + count < inputs.size() && count < static_cast<std::size_t>(batch_size_))
      {
        const std::size_t len = inputs[offset + count].size();
        if (count > 0 && chars + len > static_cast<std::size_t>(max_batch_chars_))
        {
          break;
        }
        chars += len;
        ++count;
      }
      if (count == 0)
      {
        // A single input larger than the budget still goes alone; the
        // store's chunk ceiling keeps that from being oversized.
        count = 1;
      }

      std::vector<std::string> slice(
        std::make_move_iterator(inputs.begin() + static_cast<std::ptrdiff_t>(offset)),
        std::make_move_iterator(inputs.begin() + static_cast<std::ptrdiff_t>(offset + count))
      );

      std::vector<std::vector<float>> part = co_await embed_once(std::move(slice), error);
      if (!error.empty())
      {
        result.clear();
        co_return result;
      }
      result.insert(result.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
      offset += count;
    }

    co_return result;
  }

  dripcore::awaitable<std::vector<std::vector<float>>> mistral_embedder::embed_once(
    std::vector<std::string> inputs,
    std::string& error
  )
  {
    std::vector<std::vector<float>> result;

    const std::string body = build_embeddings_request(model_, inputs, output_dimension_, output_dtype_);

    for (int attempt = 1; attempt <= k_max_attempts; ++attempt)
    {
      error.clear();
      result.clear();

      auto response_ch = std::make_shared<dripcore::channel<api::api_event>>(8);

      // The request channel is bounded, and with several requests in
      // flight it can be momentarily full. Wait for room rather than
      // reporting a spurious "closed" error: try_send() returns false for
      // both conditions and only is_closed() tells them apart.
      while (!request_ch_->try_send(api::api_request{
        .path = "/" + api_version_ + "/embeddings",
        .body = body,
        .stream = false,
        .response_ch = response_ch,
      }))
      {
        if (request_ch_->is_closed())
        {
          error = "embeddings request channel is closed";
          co_return result;
        }
        co_await dripcore::sleep_awaiter{dripcore::executor::current(), nullptr, k_poll_interval};
      }

      // A non-streaming request yields exactly one data event carrying the
      // whole response body, then done; failures arrive as one error event.
      //
      // Drained with try_recv() plus a deadline rather than recv(). The
      // transport has no timeout of its own: a stalled HTTP/2 stream never
      // reaches stream_closed, so stream_response_state::finalize() never
      // runs and a blocking recv() waits forever. That is not theoretical —
      // /index hung for minutes on an ESTABLISHED connection with no
      // progress. Polling against a deadline turns that into an error.
      const auto deadline = std::chrono::steady_clock::now() + timeout_;
      std::string response_body;
      while (true)
      {
        if (auto evt = response_ch->try_recv())
        {
          if (evt->type == api::event_type::done)
          {
            break;
          }
          if (evt->type == api::event_type::error)
          {
            error = evt->payload;
            break;
          }
          response_body += evt->payload;
          continue;
        }

        // Closed with nothing buffered: the coordinator gave up without
        // sending an event. Report it instead of waiting out the deadline.
        if (response_ch->is_closed() && response_ch->empty())
        {
          error = "embeddings response channel closed without a response";
          break;
        }

        if (std::chrono::steady_clock::now() >= deadline)
        {
          error = std::format(
            "embeddings request timed out after {} ms ({} inputs)", timeout_.count(), inputs.size()
          );
          break;
        }

        co_await dripcore::sleep_awaiter{dripcore::executor::current(), nullptr, k_poll_interval};
      }

      if (error.empty() && !parse_embeddings_response(response_body, inputs.size(), result, error))
      {
        result.clear();
      }

      if (error.empty())
      {
        co_return result;
      }

      // A timeout or a malformed response will not improve on retry; a
      // dropped connection will, because the coordinator reconnects.
      if (attempt == k_max_attempts || !is_retryable_transport_error(error))
      {
        co_return result;
      }

      co_await dripcore::sleep_awaiter{
        dripcore::executor::current(), nullptr, std::chrono::milliseconds(k_retry_backoff_ms * attempt)
      };
    }

    co_return result;
  }

} // namespace mistral::knowledge
