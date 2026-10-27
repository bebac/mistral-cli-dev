#include "agent/agent_system.h"
#include "agent/policy_engine.h"
#include "agent/tool_registry.h"
#include "http/tls_connection.h"
#include "mcp/mcp_client.h"
#include "mcp/mcp_manager.h"
#include "mcp/mcp_tool.h"
#include <doctest/doctest.h>

#include <dripcore/scheduler.h>
#include <dripcore/socket.h>
#include <dripcore/task.h>

#include <chrono>
#include <iostream>
#include <map>
#include <string>

namespace
{
  bool storybook_server_available()
  {
    try
    {
      auto addr = mistral::net::address_info::resolve("127.0.0.1", "6006");
      if (!addr)
      {
        return false;
      }
      for (auto* res : *addr)
      {
        try
        {
          dripcore::socket sock{res->ai_family, res->ai_socktype, res->ai_protocol};
          sock.connect(res->ai_addr, res->ai_addrlen);
          return true;
        }
        catch (...)
        {
          // Try the next resolved address.
        }
      }
      return false;
    }
    catch (...)
    {
      return false;
    }
  }

  class mcp_manager_live_task : public dripcore::task
  {
  public:
    mcp_manager_live_task(nlohmann::ordered_json& out, std::string& err) : out_(out), err_(err)
    {
    }

    dripcore::awaitable<> body() override
    {
      try
      {
        auto& mgr = mistral::mcp::mcp_manager::instance();
        mistral::agent::mcp_server_config storybook_cfg;
        storybook_cfg.url = "http://127.0.0.1:6006/mcp";
        mgr.configure({{"storybook", storybook_cfg}});

        auto result = co_await mgr.connect("storybook");
        out_ = result;

        // End-to-end: dispatch a tool through the global registry,
        // which the manager registered the mcp_tool bridges into.
        nlohmann::ordered_json tool_call = {
          {"function", {{"name", "get-storybook-story-instructions"}, {"arguments", "{}"}}}
        };
        auto call = co_await mistral::agent::agent_system::instance().global_tools().dispatch(tool_call);
        out_["call_ok"] = call.contains("content");

        // Policy: an "mcp:storybook" TOOL rule covers the connected
        // server's tools.
        mistral::agent::policy_engine policy;
        policy.load_configuration(
          mistral::agent::json{
            {"default", "deny"},
            {"allow", mistral::agent::json::array({mistral::agent::json{{"type", "tool"}, {"tool", "mcp:storybook"}}})}
          }
        );
        mistral::agent::policy_context ctx{
          .tool_name = "stories-preview", .tool_arguments = mistral::agent::json::object(), .working_directory = "."
        };
        auto decision = policy.evaluate(ctx);
        out_["policy_allows"] = decision.decision == mistral::agent::policy_decision::ALLOW;
      }
      catch (const std::exception& e)
      {
        err_ = e.what();
      }
      co_return;
    }

  private:
    nlohmann::ordered_json& out_;
    std::string& err_;
  };
} // namespace

TEST_CASE("mcp_tool: forwards remote schema verbatim")
{
  nlohmann::ordered_json schema;
  schema["type"] = "object";
  schema["$schema"] = "http://json-schema.org/draft-07/schema#";
  schema["properties"] = {
    {"stories",
     {{"type", "array"},
      {"items",
       {{"anyOf",
         nlohmann::ordered_json::array(
           {{{"type", "object"}, {"required", nlohmann::ordered_json::array({"storyId"})}},
            {{"type", "object"}, {"required", nlohmann::ordered_json::array({"exportName"})}}}
         )}}}}}
  };
  schema["required"] = nlohmann::ordered_json::array({"stories"});

  auto client = std::make_shared<mistral::mcp::mcp_client>("http://127.0.0.1:1/mcp");
  mistral::mcp::mcp_tool tool(client, "stories-preview", "Get preview URLs", schema);

  auto s = tool.get_schema("stories-preview", false);
  REQUIRE(s.contains("function"));
  CHECK(s["function"]["name"] == "stories-preview");
  CHECK(s["function"]["description"] == "Get preview URLs");
  // Verbatim passthrough, including $schema and anyOf unions.
  CHECK(s["function"]["parameters"] == schema);

  // Clone preserves the bridge; strict mode is ignored for MCP tools.
  auto cloned = tool.clone();
  auto s2 = cloned->get_schema("stories-preview", true);
  CHECK(s2["function"]["parameters"] == schema);
}

TEST_CASE("mcp_manager: configure and status without connection")
{
  auto& mgr = mistral::mcp::mcp_manager::instance();
  mistral::agent::mcp_server_config cfg_a;
  cfg_a.url = "http://127.0.0.1:1/mcp";
  mistral::agent::mcp_server_config cfg_b;
  cfg_b.url = "http://127.0.0.1:2/mcp";
  mgr.configure({{"a", cfg_a}, {"b", cfg_b}});

  auto status = mgr.status();
  REQUIRE(status.size() == 2);
  CHECK(status[0]["name"] == "a");
  CHECK(status[0]["url"] == "http://127.0.0.1:1/mcp");
  CHECK(status[0]["connected"] == false);
  CHECK(status[1]["name"] == "b");
  CHECK(mgr.tool_names("*").empty());
  CHECK(mgr.tool_names("a").empty());
  CHECK(mgr.instructions().empty());
}

TEST_CASE("mcp_manager: live connect registers tools and dispatches")
{
  if (!storybook_server_available())
  {
    std::cout << "  (skipping: storybook dev server not on 127.0.0.1:6006)\n";
    return;
  }

  nlohmann::ordered_json result;
  std::string error;
  dripcore::scheduler scheduler;
  scheduler.spawn<mcp_manager_live_task>(result, error);
  scheduler.run();

  REQUIRE(error.empty());
  REQUIRE(result.contains("connected"));
  CHECK(result["connected"] == true);
  CHECK(result["tools"] == 4);
  CHECK(result["call_ok"] == true);
  CHECK(result["policy_allows"] == true);

  // The manager registered the tools under their remote names.
  auto& mgr = mistral::mcp::mcp_manager::instance();
  auto names = mgr.tool_names("*");
  REQUIRE(names.size() == 4);
  CHECK(names[0] == "stories-preview");
}
