#include "mcp/mcp_client.h"

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

    std::string find_header(const http::headers& h, std::string_view name)
    {
      for (const auto& hdr : h.get()) {
        if (hdr.name == name) {
          return hdr.value;
        }
      }
      return {};
    }
  } // namespace

  mcp_client::mcp_client(
    std::string endpoint_url, std::chrono::seconds request_timeout)
    : endpoint_url_(std::move(endpoint_url))
    , timeout_(request_timeout)
  {
  }

  void mcp_client::ensure_endpoint_parsed()
  {
    if (!host_.empty()) {
      return;
    }

    auto parsed = uri::parse(endpoint_url_);
    if (auto* err = std::get_if<uri::parse_error>(&parsed)) {
      throw std::runtime_error("invalid mcp endpoint url '" +
        endpoint_url_ + "': " + std::string(err->message));
    }
    auto& u = std::get<uri::uri_view>(parsed);
    if (u.scheme() != "http") {
      throw std::runtime_error("unsupported scheme in mcp endpoint '" +
        endpoint_url_ + "': only http is supported");
    }
    host_ = std::string(u.host());
    if (host_.empty()) {
      throw std::runtime_error("mcp endpoint has no host: " + endpoint_url_);
    }
    port_ = u.port().empty()
      ? 80
      : static_cast<uint16_t>(std::stoi(std::string(u.port())));
    path_ = u.path().empty() ? "/" : std::string(u.path());
    host_header_ = (port_ == 80) ? host_ : host_ + ":" + std::to_string(port_);
  }

  dripcore::awaitable<bool> mcp_client::connect()
  {
    ensure_endpoint_parsed();
    try {
      http1_.connect(host_, port_);
    } catch (const std::exception&) {
      co_return false;
    }
    co_return true;
  }

  // Sends one JSON-RPC message and pumps the I/O loop until the
  // matching response arrives (or the request times out). Returns the
  // full JSON-RPC response object. Throws std::runtime_error on
  // transport failures.
  dripcore::awaitable<json> mcp_client::post(
    const json& message, int expect_id)
  {
    if (!http1_.connected()) {
      if (!(co_await connect())) {
        throw std::runtime_error("cannot connect to " + host_ + ":" +
          std::to_string(port_));
      }
    }

    std::string body = message.dump();
    http::headers h;
    h.add("Host", host_header_);
    h.add("Content-Type", "application/json");
    h.add("Accept", "application/json, text/event-stream");
    if (!session_id_.empty()) {
      h.add("Mcp-Session-Id", session_id_);
    }
    http1_.send_request("POST", path_, h,
      std::as_bytes(std::span<const char>(body.data(), body.size())));

    auto deadline = std::chrono::steady_clock::now() + timeout_;

    int status = 0;
    std::string content_type;
    std::vector<std::byte> response_body;
    bool complete = false;
    bool closed = false;

    while (true) {
      // Drain parser events.
      while (http1_.has_events()) {
        auto evt = http1_.next_event();
        switch (evt->type) {
          case http1_event_type::response_headers: {
            status = evt->status_code;
            content_type = tolower(find_header(evt->headers, "content-type"));
            auto sid = find_header(evt->headers, "mcp-session-id");
            if (!sid.empty()) {
              session_id_ = sid;
            }
            break;
          }
          case http1_event_type::data:
            response_body.insert(
              response_body.end(), evt->data.begin(), evt->data.end());
            break;
          case http1_event_type::message_complete:
            complete = true;
            break;
          case http1_event_type::connection_closed:
            closed = true;
            break;
        }
      }
      if (complete || closed) {
        break;
      }

      // Write pending request bytes.
      auto out = http1_.output_begin();
      if (!out.empty()) {
        int n = http1_.write(out);
        if (n > 0) {
          http1_.output_end(static_cast<size_t>(n));
          continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
          http1_.close();
          throw std::runtime_error(
            std::format("mcp write failed: {}", std::strerror(errno)));
        }
      }

      // Read response bytes.
      auto in = http1_.input_begin();
      int n = http1_.read(in);
      if (n > 0) {
        http1_.input_end(static_cast<size_t>(n));
        continue;
      }
      if (n == 0) {
        http1_.eof();
        continue;
      }
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        http1_.close();
        throw std::runtime_error(
          std::format("mcp read failed: {}", std::strerror(errno)));
      }

      // Nothing progressed: check the deadline and yield a timeslice.
      // (Same poll pattern as exec_tools - io_awaiter has no timeout
      // support, and MCP round trips are few and small.)
      if (std::chrono::steady_clock::now() >= deadline) {
        http1_.close();
        throw std::runtime_error("mcp request timed out after " +
          std::to_string(timeout_.count()) + "s");
      }
      co_await dripcore::sleep_awaiter{
        dripcore::executor::current(), nullptr,
        std::chrono::milliseconds(5)};
    }

    // One request per connection (Connection: close).
    http1_.close();

    if (!complete) {
      throw std::runtime_error(
        "connection closed before the response completed");
    }
    if (status < 200 || status >= 300) {
      throw std::runtime_error(
        "HTTP " + std::to_string(status) + " from mcp endpoint");
    }

    // Notifications may produce an empty body (202 Accepted).
    if (response_body.empty()) {
      co_return json();
    }

    // Extract the JSON-RPC message. SSE bodies carry it as a data
    // event; plain JSON bodies are the message itself.
    if (content_type.find("text/event-stream") != std::string::npos) {
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
          } catch (...) {
            // Ignore malformed keep-alive events.
          }
        });
      for (auto& m : messages) {
        if (m.contains("id") && m["id"] == expect_id) {
          co_return m;
        }
      }
      throw std::runtime_error(
        "no json-rpc response with matching id in sse stream");
    }

    co_return json::parse(std::string(
      reinterpret_cast<const char*>(response_body.data()),
      response_body.size()));
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
    } catch (const std::exception&) {
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

  dripcore::awaitable<json> mcp_client::call_tool(
    const std::string& name, const json& args)
  {
    json params = {{"name", name}, {"arguments", args}};
    co_return co_await request("tools/call", params);
  }

} // namespace mistral::mcp
