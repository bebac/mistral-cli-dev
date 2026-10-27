#ifndef API_PROVIDER_BASE_H
#define API_PROVIDER_BASE_H

#include "api/provider.h"

namespace mistral::api
{
  // Partial provider implementation containing the logic shared by all
  // chat completion providers.
  //
  // Uses the template method pattern: build_chat_request and
  // parse_stream_delta implement the common request building and delta
  // parsing algorithms, and delegate the provider-specific parts to the
  // protected hooks apply_request_fields and parse_delta_fields.
  class provider_base : public provider
  {
  public:
    // Common request body construction: model, stream, messages,
    // sampling parameters, and tools. Provider-specific fields are
    // added via apply_request_fields.
    std::string build_chat_request(
      const std::string& model,
      const std::vector<message>& messages,
      const json& tools_schema,
      bool stream,
      const std::optional<std::string>& prompt_cache_key,
      const sampling_params& params
    ) const final;

    // Common SSE delta parsing: finish_reason, content (string or
    // content-array), and tool_calls. Provider-specific delta fields
    // are parsed via parse_delta_fields.
    stream_delta parse_stream_delta(const json& j) const final;

  protected:
    // Serialize messages using the provider-specific format.
    void apply_messages(json& body, const std::vector<message>& messages) const;

    // Serialize messages and apply the standard sampling parameters.
    // Used by build_chat_request.
    void apply_messages_and_sampling(
      json& body,
      const std::vector<message>& messages,
      const sampling_params& params
    ) const;
    // Hook: add provider-specific fields to the request body.
    // Default: no additional fields.
    virtual void apply_request_fields(
      json& /*body*/,
      bool /*stream*/,
      const std::optional<std::string>& /*prompt_cache_key*/,
      const sampling_params& /*params*/
    ) const
    {
    }

    // Hook: parse provider-specific delta fields (e.g. OpenAI's
    // reasoning_content). Default: no additional fields.
    virtual void parse_delta_fields(const json& /*delta*/, stream_delta& /*result*/) const
    {
    }
  };
} // namespace mistral::api

#endif // API_PROVIDER_BASE_H
