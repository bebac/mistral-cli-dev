#ifndef MISTRAL_MISTRAL_EMBEDDER_H
#define MISTRAL_MISTRAL_EMBEDDER_H

#include "knowledge/embedder.h"

#include "api/types.h"

#include <dripcore/channel.h>

#include <cstddef>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace mistral::knowledge
{
  // ---- Wire format ----
  // Both functions are pure so the request and response shapes can be
  // tested without a network or a transport.

  // Request body for POST /v1/embeddings. output_dimension <= 0 omits
  // the field (the server default); output_dtype is omitted when empty
  // or "float". EmbeddingRequest is additionalProperties: false, so no
  // other field is ever added.
  std::string build_embeddings_request(
    const std::string& model,
    const std::vector<std::string>& inputs,
    int output_dimension,
    const std::string& output_dtype
  );

  // Parse an EmbeddingResponse body into vectors ordered by
  // data[].index (the server does not guarantee array order). Returns
  // false and sets error on a malformed body, an error payload, a count
  // mismatch, a repeated or out-of-range index, or vectors of differing
  // dimensions.
  bool parse_embeddings_response(
    const std::string& body,
    std::size_t expected_count,
    std::vector<std::vector<float>>& out,
    std::string& error
  );

  // ---- Embedder ----
  // codestral-embed through the shared HTTP transport: the same
  // per-endpoint coordinator the chat requests use, so no extra
  // connection is opened when the embeddings endpoint resolves to the
  // chat endpoint's host/port/key.
  class mistral_embedder : public embedder
  {
  public:
    // Largest batch the endpoint accepts. Verified against the live
    // endpoint: 256 inputs succeed, 512 are rejected with "Too many
    // inputs in request, split into more batches."
    static constexpr int k_max_inputs = 256;

    // request_ch may be null; embed_batch() then fails with a clear
    // error instead of crashing.
    mistral_embedder(
      std::shared_ptr<dripcore::channel<api::api_request>> request_ch,
      std::string model,
      std::string api_version,
      int output_dimension,
      std::string output_dtype,
      int batch_size = 32,
      int timeout_ms = 60000,
      int max_batch_chars = 32000
    );

    int dim() const override
    {
      return output_dimension_;
    }

    std::string identity() const override;

    // Configured batch size, clamped to the endpoint's hard limit.
    int batch_size() const override
    {
      if (batch_size_ <= 0)
      {
        return k_max_inputs;
      }
      return batch_size_ < k_max_inputs ? batch_size_ : k_max_inputs;
    }

    // The endpoint limits total tokens per request as well as per input,
    // so the store groups its inputs by characters too.
    int max_batch_chars() const override
    {
      return max_batch_chars_;
    }

    dripcore::awaitable<std::vector<std::vector<float>>>
      embed_batch(std::vector<std::string> inputs, std::string& error) override;

  private:
    std::shared_ptr<dripcore::channel<api::api_request>> request_ch_;
    std::string model_;
    std::string api_version_;
    int output_dimension_;
    std::string output_dtype_;
    int batch_size_ = 32;

    // Deadline for one embed_batch() round trip. The transport has no
    // timeout of its own, so without this a stalled HTTP/2 stream hangs
    // the caller forever.
    std::chrono::milliseconds timeout_{std::chrono::seconds(60)};

    // Total characters one request may carry. The endpoint enforces a
    // per-input token limit *and* a total-token limit per request, so
    // batch_size alone is not a safe bound: 32 full-size chunks trip
    // "Too many tokens overall". ~32k characters is roughly 8-10k
    // tokens, comfortably under the ~16k practical ceiling.
    int max_batch_chars_ = 32000;

    // One request/response round trip for a slice that already fits the
    // endpoint's limits. embed_batch() splits its input across these.
    dripcore::awaitable<std::vector<std::vector<float>>> embed_once(
      std::vector<std::string> inputs,
      std::string& error
    );
  };

} // namespace mistral::knowledge

#endif // MISTRAL_MISTRAL_EMBEDDER_H
