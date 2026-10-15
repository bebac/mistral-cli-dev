#include <doctest/doctest.h>
#include "mcp/http1_client.h"
#include "mcp/mcp_client.h"

#include <dripcore/scheduler.h>
#include <dripcore/task.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{
  using mistral::mcp::http1_client;
  using mistral::mcp::http1_event_type;

  std::vector<std::byte> bytes(std::string_view s)
  {
    const auto* p = reinterpret_cast<const std::byte*>(s.data());
    return std::vector<std::byte>(p, p + s.size());
  }

  std::string str(const std::vector<std::byte>& v)
  {
    return std::string(reinterpret_cast<const char*>(v.data()), v.size());
  }

  // Feeds raw bytes into the client's parser the way the I/O loop
  // does (through input_begin/input_end).
  void feed(http1_client& c, std::string_view raw)
  {
    size_t offset = 0;
    while (offset < raw.size()) {
      auto in = c.input_begin();
      size_t take = std::min(in.size(), raw.size() - offset);
      std::memcpy(in.data(), raw.data() + offset, take);
      c.input_end(take);
      offset += take;
    }
  }

  // Prepares the client for parsing a response (send_request resets
  // the response-side parser state; no connection needed).
  void arm_response_parser(http1_client& c)
  {
    c.send_request("POST", "/mcp", {}, bytes("{}"));
    // Discard the request bytes; we only need the parser armed.
    c.output_end(c.output_begin().size());
  }
} // namespace

TEST_CASE("http1_client: serializes a POST request")
{
  http1_client c;
  http::headers h;
  h.add("Content-Type", "application/json");
  c.send_request("POST", "/mcp", h, bytes("{\"a\":1}"));

  auto out = c.output_begin();
  std::string raw(reinterpret_cast<const char*>(out.data()), out.size());
  CHECK(raw ==
    "POST /mcp HTTP/1.1\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 7\r\n"
    "Connection: close\r\n"
    "\r\n"
    "{\"a\":1}");

  c.output_end(5);
  CHECK(c.output_begin().size() == raw.size() - 5);
}

TEST_CASE("http1_client: parses response with content-length")
{
  http1_client c;
  arm_response_parser(c);
  feed(c, "HTTP/1.1 200 OK\r\n"
          "Content-Type: application/json\r\n"
          "Content-Length: 5\r\n"
          "\r\n"
          "hello");

  REQUIRE(c.has_events());
  auto e1 = c.next_event();
  REQUIRE(e1.has_value());
  CHECK(e1->type == http1_event_type::response_headers);
  CHECK(e1->status_code == 200);

  auto e2 = c.next_event();
  REQUIRE(e2.has_value());
  CHECK(e2->type == http1_event_type::data);
  CHECK(str(e2->data) == "hello");

  auto e3 = c.next_event();
  REQUIRE(e3.has_value());
  CHECK(e3->type == http1_event_type::message_complete);
  CHECK_FALSE(c.has_events());
}

TEST_CASE("http1_client: parses empty-body 202 response")
{
  http1_client c;
  arm_response_parser(c);
  feed(c, "HTTP/1.1 202 Accepted\r\n"
          "Content-Length: 0\r\n"
          "\r\n");

  auto e1 = c.next_event();
  REQUIRE(e1.has_value());
  CHECK(e1->type == http1_event_type::response_headers);
  CHECK(e1->status_code == 202);

  auto e2 = c.next_event();
  REQUIRE(e2.has_value());
  CHECK(e2->type == http1_event_type::message_complete);
  CHECK_FALSE(c.has_events());
}

TEST_CASE("http1_client: parses chunked response")
{
  http1_client c;
  arm_response_parser(c);
  feed(c, "HTTP/1.1 200 OK\r\n"
          "Transfer-Encoding: chunked\r\n"
          "\r\n"
          "5\r\nhello\r\n"
          "6\r\n world\r\n"
          "0\r\n"
          "\r\n");

  auto e1 = c.next_event();
  REQUIRE(e1.has_value());
  CHECK(e1->type == http1_event_type::response_headers);

  auto e2 = c.next_event();
  REQUIRE(e2.has_value());
  CHECK(e2->type == http1_event_type::data);
  CHECK(str(e2->data) == "hello");

  auto e3 = c.next_event();
  REQUIRE(e3.has_value());
  CHECK(e3->type == http1_event_type::data);
  CHECK(str(e3->data) == " world");

  auto e4 = c.next_event();
  REQUIRE(e4.has_value());
  CHECK(e4->type == http1_event_type::message_complete);
  CHECK_FALSE(c.has_events());
}

TEST_CASE("http1_client: close-delimited body completes on eof")
{
  http1_client c;
  arm_response_parser(c);
  feed(c, "HTTP/1.1 200 OK\r\n"
          "Content-Type: text/event-stream\r\n"
          "\r\n"
          "data: {\"id\":1}\n\n");

  auto e1 = c.next_event();
  REQUIRE(e1.has_value());
  CHECK(e1->type == http1_event_type::response_headers);

  auto e2 = c.next_event();
  REQUIRE(e2.has_value());
  CHECK(e2->type == http1_event_type::data);
  CHECK(str(e2->data) == "data: {\"id\":1}\n\n");
  CHECK_FALSE(c.has_events());

  // EOF terminates the close-delimited body.
  c.eof();
  auto e3 = c.next_event();
  REQUIRE(e3.has_value());
  CHECK(e3->type == http1_event_type::message_complete);
}

TEST_CASE("http1_client: eof before response completes signals connection_closed")
{
  http1_client c;
  arm_response_parser(c);
  feed(c, "HTTP/1.1 200 OK\r\n"
          "Content-Length: 100\r\n"
          "\r\n"
          "partial");

  c.next_event();  // response_headers
  c.next_event();  // data (partial)

  c.eof();
  auto evt = c.next_event();
  REQUIRE(evt.has_value());
  CHECK(evt->type == http1_event_type::connection_closed);
}

TEST_CASE("http1_client: parses responses fed in small pieces")
{
  http1_client c;
  arm_response_parser(c);
  feed(c, "HTTP/1.1 200 OK\r\nContent-Len");
  CHECK_FALSE(c.has_events());  // headers not complete yet
  feed(c, "gth: 5\r\n\r\nhel");
  REQUIRE(c.has_events());
  auto e1 = c.next_event();
  CHECK(e1->type == http1_event_type::response_headers);
  auto e2 = c.next_event();
  CHECK(e2->type == http1_event_type::data);
  CHECK(str(e2->data) == "hel");
  feed(c, "lo");
  auto e3 = c.next_event();
  CHECK(e3->type == http1_event_type::data);
  CHECK(str(e3->data) == "lo");
  auto e4 = c.next_event();
  CHECK(e4->type == http1_event_type::message_complete);
}

// ---- Live integration test (requires the Storybook dev server) ----

namespace
{
  bool storybook_server_available()
  {
    try {
      mistral::mcp::http1_client probe;
      probe.connect("127.0.0.1", 6006);
      probe.close();
      return true;
    } catch (...) {
      return false;
    }
  }

  class mcp_live_task : public dripcore::task
  {
  public:
    mcp_live_task(nlohmann::ordered_json& out, std::string& err)
      : out_(out), err_(err) {}

    dripcore::awaitable<> body() override
    {
      try {
        mistral::mcp::mcp_client client(
          "http://127.0.0.1:6006/mcp", std::chrono::seconds{10});

        if (!(co_await client.initialize())) {
          err_ = "initialize failed";
          co_return;
        }

        auto tools = co_await client.list_tools();
        auto call = co_await client.call_tool(
          "get-storybook-story-instructions",
          nlohmann::ordered_json::object());

        out_ = {
          {"server", client.server_info()},
          {"session_id", client.session_id()},
          {"instructions", client.instructions()},
          {"tools", tools},
          {"call", call}
        };
      } catch (const std::exception& e) {
        err_ = e.what();
      }
      co_return;
    }

  private:
    nlohmann::ordered_json& out_;
    std::string& err_;
  };
} // namespace

TEST_CASE("mcp_client: live Storybook MCP handshake and tool call")
{
  if (!storybook_server_available()) {
    std::cout << "  (skipping: storybook dev server not on 127.0.0.1:6006)\n";
    return;
  }

  nlohmann::ordered_json result;
  std::string error;
  dripcore::scheduler scheduler;
  scheduler.spawn<mcp_live_task>(result, error);
  scheduler.run();

  REQUIRE(error.empty());
  REQUIRE(result.contains("server"));
  CHECK(result["server"]["name"].get<std::string>().find("addon-mcp") !=
        std::string::npos);
  CHECK_FALSE(result["session_id"].get<std::string>().empty());
  CHECK_FALSE(result["instructions"].get<std::string>().empty());

  // Tool surface documented in doc/storybook-mcp.md.
  REQUIRE(result.contains("tools"));
  REQUIRE(result["tools"].contains("tools"));
  auto names = nlohmann::ordered_json::array();
  for (const auto& t : result["tools"]["tools"]) {
    names.push_back(t["name"]);
  }
  REQUIRE(names.size() == 4);
  CHECK(names[0].get<std::string>() == "stories-preview");
  CHECK(names[1].get<std::string>() == "get-storybook-story-instructions");
  CHECK(names[2].get<std::string>() == "stories-changed");
  CHECK(names[3].get<std::string>() == "stories-find-by-component");

  // The instructions tool returns text content.
  REQUIRE(result.contains("call"));
  REQUIRE(result["call"].contains("content"));
  REQUIRE(result["call"]["content"].size() > 0);
  CHECK_FALSE(result["call"]["content"][0]["text"].get<std::string>().empty());
}
