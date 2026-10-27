#ifndef MISTRAL_MCP_CLIENT_INTERFACE_H
#define MISTRAL_MCP_CLIENT_INTERFACE_H

#include <dripcore/awaitable.h>
#include <nlohmann/json.hpp>

#include <string>

namespace mistral::mcp
{
  using json = nlohmann::ordered_json;

  // Common interface for MCP clients (HTTP and stdio transports).
  // mcp_tool bridges a remote MCP tool into the agent tool registry
  // via this interface, so it works with either transport.
  class mcp_client_interface
  {
  public:
    virtual ~mcp_client_interface() = default;

    // MCP initialize handshake + notifications/initialized.
    virtual dripcore::awaitable<bool> initialize() = 0;

    // tools/list - returns the full result object (contains "tools").
    virtual dripcore::awaitable<json> list_tools() = 0;

    // tools/call - returns the full result object.
    virtual dripcore::awaitable<json> call_tool(const std::string& name, const json& args) = 0;

    virtual bool initialized() const = 0;
    virtual const std::string& instructions() const = 0;
  };

} // namespace mistral::mcp

#endif // MISTRAL_MCP_CLIENT_INTERFACE_H
