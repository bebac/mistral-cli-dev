#ifndef MISTRAL_MCP_TOOL_H
#define MISTRAL_MCP_TOOL_H

#include "agent/tool_registry.h"
#include "mcp/mcp_client_interface.h"

#include <cstddef>
#include <string>

namespace mistral::mcp
{
  // Maximum size of an MCP tool result handed to the model. A server can
  // return an arbitrarily large payload (a broad grep, a whole document);
  // excess is dropped and flagged rather than passed on, mirroring exec's
  // stdout/stderr cap, so a single call cannot blow the context window.
  inline constexpr std::size_t k_max_result_bytes = 64 * 1024;

  // Cap the model-facing size of a tools/call result. A result that fits
  // is returned unchanged (still the tools/call object); a larger one is
  // truncated to k_max_result_bytes and returned as a string whose tail
  // names how much was dropped.
  inline json cap_result_size(json result)
  {
    std::string serialized = result.dump();
    const std::size_t total = serialized.size();
    if (total <= k_max_result_bytes)
    {
      return result;
    }

    serialized.resize(k_max_result_bytes);
    serialized += "\n[... MCP tool result truncated: ";
    serialized += std::to_string(total - k_max_result_bytes);
    serialized += " of ";
    serialized += std::to_string(total);
    serialized += " bytes omitted ...]";
    return json(std::move(serialized));
  }

  // Dynamic tool_interface bridging a remote MCP tool into the agent
  // tool registry. The remote inputSchema is forwarded verbatim (MCP
  // schemas use draft-07 with anyOf unions that cannot be re-derived
  // from C++ types), and execute() forwards to tools/call.
  //
  // Works with any mcp_client_interface implementation (HTTP or stdio
  // transport).
  class mcp_tool : public agent::tool_interface
  {
  public:
    mcp_tool(std::shared_ptr<mcp_client_interface> client, std::string name, std::string description, json input_schema)
      : client_(std::move(client)), name_(std::move(name)), description_(std::move(description)),
        input_schema_(std::move(input_schema))
    {
    }

    dripcore::awaitable<json> execute(const json& args) override
    {
      // Returns the tools/call result object (content[] and, when
      // declared, structuredContent), capped to k_max_result_bytes so a
      // server cannot flood the context. Exceptions propagate to
      // tool_registry::dispatch, which converts them to error strings.
      json result = co_await client_->call_tool(name_, args);
      co_return cap_result_size(std::move(result));
    }

    json get_schema(const std::string& tool_name, bool /*strict*/) override
    {
      // Remote schemas are passed through verbatim. Strict mode is
      // intentionally ignored: MCP schemas cannot satisfy strict-mode
      // requirements (e.g. every property listed in "required").
      json function = {
        {"name", tool_name},
        {"description", description_},
        {"parameters", input_schema_.is_object() ? input_schema_ : json::object()}
      };
      return {{"type", "function"}, {"function", function}};
    }

    std::shared_ptr<agent::tool_interface> clone() const override
    {
      return std::make_shared<mcp_tool>(client_, name_, description_, input_schema_);
    }

  private:
    std::shared_ptr<mcp_client_interface> client_;
    std::string name_;
    std::string description_;
    json input_schema_;
  };

} // namespace mistral::mcp

#endif // MISTRAL_MCP_TOOL_H
