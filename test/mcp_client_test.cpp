#include <doctest/doctest.h>
#include "mcp/mcp_client.h"

#include "http/tls_connection.h"

#include <dripcore/scheduler.h>
#include <dripcore/socket.h>
#include <dripcore/task.h>

#include <chrono>
#include <iostream>
#include <string>

// ---- Live integration test (requires the Storybook dev server) ----

namespace
{
  bool storybook_server_available()
  {
    try {
      auto addr = mistral::net::address_info::resolve("127.0.0.1", "6006");
      if (!addr) {
        return false;
      }
      for (auto* res : *addr) {
        try {
          dripcore::socket sock{res->ai_family, res->ai_socktype,
                                res->ai_protocol};
          sock.connect(res->ai_addr, res->ai_addrlen);
          return true;
        } catch (...) {
          // Try the next resolved address.
        }
      }
      return false;
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
