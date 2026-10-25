#include <doctest/doctest.h>
#include "mcp/mcp_stdio_client.h"
#include "http/tls_connection.h"

#include <dripcore/scheduler.h>
#include <dripcore/socket.h>
#include <dripcore/task.h>

#include <chrono>
#include <iostream>
#include <string>

namespace
{
  bool xerj_node_available()
  {
    // Quick TCP connect check to localhost:9200.
    try {
      auto addr = mistral::net::address_info::resolve("127.0.0.1", "9200");
      if (!addr) return false;
      for (auto* res : *addr) {
        try {
          dripcore::socket sock{res->ai_family, res->ai_socktype,
                                res->ai_protocol};
          sock.connect(res->ai_addr, res->ai_addrlen);
          return true;
        } catch (...) {}
      }
      return false;
    } catch (...) {
      return false;
    }
  }

  class stdio_test_task : public dripcore::task
  {
  public:
    stdio_test_task(nlohmann::ordered_json& out, std::string& err)
      : out_(out), err_(err) {}

    dripcore::awaitable<> body() override
    {
      try {
        mistral::mcp::mcp_stdio_client client(
          "xerj", std::vector<std::string>{"mcp"},
          std::map<std::string, std::string>{
            {"XERJ_URL", "http://localhost:9200"}},
          std::chrono::seconds{15});

        if (!(co_await client.connect())) {
          err_ = "connect failed (is xerj installed?)";
          co_return;
        }
        out_["connected"] = true;

        if (!(co_await client.initialize())) {
          err_ = "initialize failed";
          co_return;
        }
        out_["initialized"] = true;
        out_["server_info"] = client.server_info();

        auto tools = co_await client.list_tools();
        if (tools.contains("tools") && tools["tools"].is_array()) {
          out_["tool_count"] = tools["tools"].size();
          nlohmann::ordered_json names = nlohmann::ordered_json::array();
          for (const auto& t : tools["tools"]) {
            if (t.contains("name")) {
              names.push_back(t["name"].get<std::string>());
            }
          }
          out_["tool_names"] = names;
        }

        // Try calling xerj_map (no args needed).
        auto result = co_await client.call_tool("xerj_map", nlohmann::ordered_json::object());
        out_["map_ok"] = result.contains("content") || result.contains("structuredContent");
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

TEST_CASE("mcp_stdio_client: connect to xerj mcp and list tools")
{
  if (!xerj_node_available()) {
    std::cout << "  (skipping: xerj node not on localhost:9200)\n";
    return;
  }

  nlohmann::ordered_json result;
  std::string error;
  dripcore::scheduler scheduler;
  scheduler.spawn<stdio_test_task>(result, error);
  scheduler.run();

  INFO("error: " << error);
  REQUIRE(error.empty());
  CHECK(result["connected"] == true);
  CHECK(result["initialized"] == true);
  REQUIRE(result.contains("tool_count"));
  CHECK(result["tool_count"].get<int>() > 0);
  CHECK(result["map_ok"] == true);

  // Verify we got the expected xerj tools.
  REQUIRE(result.contains("tool_names"));
  bool has_search = false;
  bool has_code_search = false;
  for (const auto& name : result["tool_names"]) {
    std::string n = name.get<std::string>();
    if (n == "xerj_search") has_search = true;
    if (n == "xerj_code_search") has_code_search = true;
  }
  CHECK(has_search);
  CHECK(has_code_search);
}