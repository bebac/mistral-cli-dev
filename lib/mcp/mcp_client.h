#ifndef MISTRAL_MCP_CLIENT_H
#define MISTRAL_MCP_CLIENT_H

#include <http/client_context.h>

#include <dripcore/awaitable.h>
#include <dripcore/socket.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace mistral::mcp
{
  using json = nlohmann::ordered_json;

  // MCP client for the Streamable HTTP transport (one JSON-RPC message
  // per HTTP POST; responses arrive as application/json or as a
  // text/event-stream body carrying a single JSON-RPC message).
  //
  // Wire layer is stunning-waddle's http::client_context speaking
  // HTTP/1.1 (protocol_version::v1_1) over a plain dripcore socket.
  //
  // Contract notes for the Storybook dev server endpoint are in
  // doc/storybook-mcp.md.
  class mcp_client
  {
  public:
    explicit mcp_client(
      std::string endpoint_url,
      std::chrono::seconds request_timeout = std::chrono::seconds{30});

    // Parses the endpoint URL and opens a TCP connection.
    // Returns false when the server is unreachable.
    dripcore::awaitable<bool> connect();

    // MCP initialize handshake + notifications/initialized.
    // On success, session_id()/server_info()/instructions() are
    // populated. Throws std::runtime_error on failure.
    dripcore::awaitable<bool> initialize();

    bool initialized() const { return !session_id_.empty(); }
    const std::string& session_id() const { return session_id_; }
    const json& server_info() const { return server_info_; }
    const std::string& instructions() const { return instructions_; }

    // tools/list - returns the full result object (contains "tools").
    dripcore::awaitable<json> list_tools();

    // tools/call - returns the full result object (content[] and,
    // when the tool declares an outputSchema, structuredContent).
    dripcore::awaitable<json> call_tool(
      const std::string& name, const json& args);

    // Generic JSON-RPC request. Returns the "result" member; throws
    // std::runtime_error on transport failure or JSON-RPC error.
    dripcore::awaitable<json> request(
      const std::string& method, const json& params);

    // JSON-RPC notification (no response expected).
    dripcore::awaitable<bool> notify(const std::string& method);

  private:
    dripcore::awaitable<json> post(const json& message, int expect_id);
    void ensure_endpoint_parsed();
    void close();

    std::string endpoint_url_;
    std::string host_;
    uint16_t port_ = 0;
    std::string path_;
    std::string host_header_;
    std::chrono::seconds timeout_;
    std::optional<dripcore::socket> socket_;
    std::unique_ptr<http::client_context> ctx_;
    std::string session_id_;
    json server_info_ = json::object();
    std::string instructions_;
    int next_id_ = 1;
  };

} // namespace mistral::mcp

#endif // MISTRAL_MCP_CLIENT_H
