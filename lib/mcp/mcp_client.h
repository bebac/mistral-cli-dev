#ifndef MISTRAL_MCP_CLIENT_H
#define MISTRAL_MCP_CLIENT_H

#include "mcp/mcp_client_interface.h"

#include <http/client_context.h>

#include <dripcore/awaitable.h>
#include <dripcore/socket.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

// Forward declarations: the TLS state is held via unique_ptr and the
// destructor is defined out-of-line in mcp_client.cpp, so the OpenSSL
// headers stay out of this header's consumers.
namespace mistral::net
{
  class ssl_context;
  class ssl_handle;
}

namespace mistral::mcp
{
  // MCP client for the Streamable HTTP transport (one JSON-RPC message
  // per HTTP POST; responses arrive as application/json or as a
  // text/event-stream body carrying a single JSON-RPC message).
  //
  // Wire layer is stunning-waddle's http::client_context speaking
  // HTTP/1.1 (protocol_version::v1_1) over a plain dripcore socket
  // (http endpoints) or over TLS via OpenSSL (https endpoints; no
  // ALPN is offered so the server defaults to HTTP/1.1).
  //
  // Contract notes for the Storybook dev server endpoint are in
  // doc/storybook-mcp.md.
  class mcp_client : public mcp_client_interface
  {
  public:
    explicit mcp_client(std::string endpoint_url, std::chrono::seconds request_timeout = std::chrono::seconds{30});
    ~mcp_client() override;

    // Parses the endpoint URL and opens a TCP connection (plus the TLS
    // handshake for https endpoints). Returns false when the server is
    // unreachable; last_connect_error() then holds the reason.
    dripcore::awaitable<bool> connect();

    // Reason for the most recent connect() failure, empty after a
    // successful connect(). Covers DNS, TCP and TLS failures so callers
    // can report why instead of a generic "unreachable".
    const std::string& last_connect_error() const
    {
      return connect_error_;
    }

    // MCP initialize handshake + notifications/initialized.
    // On success, session_id()/server_info()/instructions() are
    // populated. Throws std::runtime_error on failure.
    dripcore::awaitable<bool> initialize() override;

    bool initialized() const override
    {
      return !session_id_.empty();
    }
    const std::string& session_id() const
    {
      return session_id_;
    }
    const json& server_info() const
    {
      return server_info_;
    }
    const std::string& instructions() const override
    {
      return instructions_;
    }

    // tools/list - returns the full result object (contains "tools").
    dripcore::awaitable<json> list_tools() override;

    // tools/call - returns the full result object (content[] and,
    // when the tool declares an outputSchema, structuredContent).
    dripcore::awaitable<json> call_tool(const std::string& name, const json& args) override;

    // Generic JSON-RPC request. Returns the "result" member; throws
    // std::runtime_error on transport failure or JSON-RPC error.
    dripcore::awaitable<json> request(const std::string& method, const json& params);

    // JSON-RPC notification (no response expected).
    dripcore::awaitable<bool> notify(const std::string& method);

  private:
    dripcore::awaitable<json> post(const json& message, int expect_id);
    void ensure_endpoint_parsed();
    void close();

    // Transport I/O: dispatches to TLS when the endpoint is https, to
    // the plain socket otherwise. SSL want-read/want-write is mapped
    // to EAGAIN so the pump loop's existing retry logic applies
    // unchanged; fatal TLS errors are mapped to EIO so the loop fails
    // fast instead of retrying on a stale errno.
    ssize_t stream_send(const void* buf, size_t len);
    ssize_t stream_recv(void* buf, size_t len);

    std::string endpoint_url_;
    std::string host_;
    uint16_t port_ = 0;
    std::string path_;
    std::string host_header_;
    std::chrono::seconds timeout_;
    std::optional<dripcore::socket> socket_;
    std::unique_ptr<http::client_context> ctx_;
    bool use_tls_ = false;
    // Per-client TLS context: created on the first https connect and
    // reused for every later connection. Declared before ssl_ so that
    // the handle is destroyed first (SSL_free before SSL_CTX_free).
    std::unique_ptr<net::ssl_context> ssl_ctx_;
    std::unique_ptr<net::ssl_handle> ssl_;
    std::string connect_error_;
    std::string session_id_;
    json server_info_ = json::object();
    std::string instructions_;
    int next_id_ = 1;
  };

} // namespace mistral::mcp

#endif // MISTRAL_MCP_CLIENT_H
