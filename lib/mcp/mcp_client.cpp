#include "mcp/mcp_client.h"

#include "http/tls_connection.h"

#include <dripcore/executor.h>
#include <dripcore/sleep_awaiter.h>
#include <http/sse.h>
#include <uri.h>

#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstring>
#include <format>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace mistral::mcp
{
  namespace
  {
    std::string tolower(std::string s)
    {
      for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      return s;
    }
  } // namespace

  mcp_client::mcp_client(std::string endpoint_url, std::chrono::seconds request_timeout)
      : endpoint_url_(std::move(endpoint_url)), timeout_(request_timeout)
  {
  }

  void mcp_client::ensure_endpoint_parsed()
  {
    if (!host_.empty()) {
      return;
    }

    auto parsed = uri::parse(endpoint_url_);
    if (auto* err = std::get_if<uri::parse_error>(&parsed)) {
      throw std::runtime_error("invalid mcp endpoint url '" + endpoint_url_ + "': " + std::string(err->message));
    }
    auto& u = std::get<uri::uri_view>(parsed);
    if (u.scheme() != "http") {
      throw std::runtime_error("unsupported scheme in mcp endpoint '" + endpoint_url_ + "': only http is supported");
    }
    host_ = std::string(u.host());
    if (host_.empty()) {
      throw std::runtime_error("mcp endpoint has no host: " + endpoint_url_);
    }
    port_ = u.port().empty() ? 80 : static_cast<uint16_t>(std::stoi(std::string(u.port())));
    path_ = u.path().empty() ? "/" : std::string(u.path());
    host_header_ = (port_ == 80) ? host_ : host_ + ":" + std::to_string(port_);
  }

  dripcore::awaitable<bool> mcp_client::connect()
  {
    ensure_endpoint_parsed();
    close();
    try
    {
      auto addr = net::address_info::resolve(host_, std::to_string(port_));
      if (!addr) {
        co_return false;
      }
      for (auto* res : *addr) {
        try {
          dripcore::socket sock{res->ai_family, res->ai_socktype,
                                res->ai_protocol};
          sock.connect(res->ai_addr, res->ai_addrlen);
          sock.nonblocking(true);
          socket_ = std::move(sock);
          break;
        }
        catch (const std::exception&) {
          // Try the next resolved address.
        }
      }
      if (!socket_) {
        co_return false;
      }
      ctx_ = std::make_unique<http::client_context>();
      ctx_->set_protocol_version(http::protocol_version::v1_1);
      co_return true;
    }
    catch (...) {
      co_return false;
    }
  }

  void mcp_client::close()
  {
    socket_.reset();
    ctx_.reset();
  }

  // Sends one JSON-RPC message and pumps the I/O loop until the
  // matching response arrives (or the request times out). Returns the
  // full JSON-RPC response object. Throws std::runtime_error on
  // transport failures.
  dripcore::awaitable<json> mcp_client::post(const json& message, int expect_id)
  {
    if (!socket_) {
      if (!(co_await connect())) {
        throw std::runtime_error("cannot connect to " + host_ + ":" + std::to_string(port_));
      }
    }

    std::string body = message.dump();
    http::headers h;
    h.add("Content-Type", "application/json");
    h.add("Accept", "application/json, text/event-stream");
    h.add("Content-Length", std::to_string(body.size()));
    h.add("Connection", "close");
    if (!session_id_.empty()) {
      h.add("Mcp-Session-Id", session_id_);
    }
    auto s = ctx_->open_stream();
    s.send_headers("POST", path_, host_header_, h, false);
    s.send_data(std::as_bytes(std::span<const char>(body.data(), body.size())), true);

    auto deadline = std::chrono::steady_clock::now() + timeout_;

    int status = 0;
    std::string content_type;
    std::vector<std::byte> response_body;
    bool complete = false;
    bool closed = false;

    while (true)
    {
      // Drain protocol events.
      while (ctx_->has_events())
      {
        auto evt = ctx_->next_event();
        if (!evt) {
          break;
        }
        switch (evt->type) {
          case http::event_type::headers: {
            const auto& hdrs = std::get<http::headers>(evt->payload);
            status = std::stoi(hdrs.get(":status"));
            content_type = tolower(hdrs.get("content-type"));
            auto sid = hdrs.get("mcp-session-id");
            if (!sid.empty()) {
              session_id_ = sid;
            }
            break;
          }
          case http::event_type::data: {
            const auto& data = std::get<std::vector<std::byte>>(evt->payload);
            response_body.insert(
              response_body.end(), data.begin(), data.end());
            break;
          }
          case http::event_type::stream_closed:
            complete = true;
            break;
          case http::event_type::connection_error:
            closed = true;
            break;
          default:
            break;  // goaway / window events: nothing to do
        }
      }
      if (complete || closed) {
        break;
      }

      // Write pending request bytes.
      auto out = ctx_->output_begin();
      if (!out.empty())
      {
        int n = static_cast<int>(socket_->send(out.data(), out.size(), 0));
        if (n > 0)
        {
          ctx_->output_end(static_cast<size_t>(n));
          continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
          close();
          throw std::runtime_error(std::format("mcp write failed: {}", std::strerror(errno)));
        }
      }

      // Read response bytes (skipped when the input buffer is full of
      // unparsed data - the engine signals an error in that case).
      auto in = ctx_->input_begin();
      if (!in.empty())
      {
        int n = static_cast<int>(socket_->recv(in.data(), in.size(), 0));
        if (n > 0) {
          ctx_->input_end(static_cast<size_t>(n));
          continue;
        }
        if (n == 0) {
          ctx_->eof();
          continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          close();
          throw std::runtime_error(std::format("mcp read failed: {}", std::strerror(errno)));
        }
      }

      // Nothing progressed: check the deadline and yield a timeslice.
      // (Same poll pattern as exec_tools - io_awaiter has no timeout
      // support, and MCP round trips are few and small.)
      if (std::chrono::steady_clock::now() >= deadline)
      {
        close();
        throw std::runtime_error("mcp request timed out after " + std::to_string(timeout_.count()) + "s");
      }
      co_await dripcore::sleep_awaiter{
        dripcore::executor::current(),
        nullptr,
        std::chrono::milliseconds(5)
      };
    }

    // One request per connection (Connection: close).
    close();

    if (!complete) {
      throw std::runtime_error("connection closed before the response completed");
    }
    if (status < 200 || status >= 300) {
      throw std::runtime_error("HTTP " + std::to_string(status) + " from mcp endpoint");
    }

    // Notifications may produce an empty body (202 Accepted).
    if (response_body.empty()) {
      co_return json();
    }

    // Extract the JSON-RPC message. SSE bodies carry it as a data
    // event; plain JSON bodies are the message itself.
    if (content_type.find("text/event-stream") != std::string::npos)
    {
      http::sse_parser parser;
      std::vector<json> messages;
      parser.parse(
        std::span<const std::byte>(response_body.data(), response_body.size()),
        [&](http::sse_event evt) {
          if (evt.data.empty()) {
            return;
          }
          try {
            messages.push_back(json::parse(evt.data));
          }
          catch (...) {
            // Ignore malformed keep-alive events.
          }
        }
      );
      for (auto& m : messages) {
        if (m.contains("id") && m["id"] == expect_id) {
          co_return m;
        }
      }
      throw std::runtime_error("no json-rpc response with matching id in sse stream");
    }

    // std::byte* converts to void* implicitly, so the copy needs no
    // reinterpret_cast (and json::parse's byte-iterator overload would
    // pull in the deprecated char_traits<std::byte>).
    std::string body_text(response_body.size(), '\0');
    if (!body_text.empty()) {
      std::memcpy(body_text.data(), response_body.data(), body_text.size());
    }
    co_return json::parse(body_text);
  }

  dripcore::awaitable<json> mcp_client::request(
    const std::string& method, const json& params)
  {
    int id = next_id_++;
    json message = {
      {"jsonrpc", "2.0"},
      {"id", id},
      {"method", method}
    };
    if (!params.is_null()) {
      message["params"] = params;
    }
    json response = co_await post(message, id);

    if (response.contains("error")) {
      const auto& err = response["error"];
      throw std::runtime_error(std::format("mcp error {}: {}",
        err.value("code", 0), err.value("message", "unknown error")));
    }
    co_return response.value("result", json::object());
  }

  dripcore::awaitable<bool> mcp_client::notify(const std::string& method)
  {
    json message = {
      {"jsonrpc", "2.0"},
      {"method", method}
    };
    try {
      co_await post(message, -1);
    }
    catch (const std::exception&) {
      co_return false;
    }
    co_return true;
  }

  dripcore::awaitable<bool> mcp_client::initialize()
  {
    json params = {
      {"protocolVersion", "2025-06-18"},
      {"capabilities", json::object()},
      {"clientInfo", {{"name", "mistral-cli"}, {"version", "0.1.0"}}}
    };
    json result = co_await request("initialize", params);

    if (result.contains("serverInfo")) {
      server_info_ = result["serverInfo"];
    }
    instructions_ = result.value("instructions", std::string());

    co_await notify("notifications/initialized");
    co_return true;
  }

  dripcore::awaitable<json> mcp_client::list_tools()
  {
    co_return co_await request("tools/list", json());
  }

  dripcore::awaitable<json> mcp_client::call_tool(const std::string& name, const json& args)
  {
    json params = {{"name", name}, {"arguments", args}};
    co_return co_await request("tools/call", params);
  }

} // namespace mistral::mcp
