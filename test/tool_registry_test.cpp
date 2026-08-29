#include <doctest/doctest.h>
#include "agent/tool_registry.h"

#include <dripcore/scheduler.h>
#include <dripcore/task.h>

#include <string>
#include <memory>

namespace mistral::agent
{
  // Test tool class - methods return json objects
  class test_tool
  {
  public:
    json add(int a, int b) { return {{"result", a + b}}; }
    json greet(std::string name) { return {{"result", "Hello, " + name}}; }
    json multiply(int a, int b) { return {{"result", a * b}}; }
  };

  // Helper to run an async dispatch call from a test
  inline json run_async_dispatch(tool_registry& registry, json tool_call)
  {
    json result;
    bool completed = false;

    // Task that performs the async dispatch and stores the result
    class dispatch_task : public dripcore::task
    {
    public:
      dispatch_task(tool_registry& reg, json tc, json& out_result, bool& out_completed)
        : registry_(reg), tool_call_(std::move(tc)), out_result_(out_result), out_completed_(out_completed) {}

      dripcore::awaitable<> body() override
      {
        out_result_ = co_await registry_.dispatch(tool_call_);
        out_completed_ = true;
        co_return;
      }

    private:
      tool_registry& registry_;
      json tool_call_;
      json& out_result_;
      bool& out_completed_;
    };

    dripcore::scheduler scheduler;
    scheduler.spawn<dispatch_task>(registry, std::move(tool_call), result, completed);
    scheduler.run();

    if (completed) {
      return result;
    }
    return {};
  }

} // namespace mistral::agent

TEST_CASE("tool_registry: register and dispatch sync-like tool")
{
  using namespace mistral::agent;

  tool_registry registry;
  test_tool tool_inst;

  registry.register_tool(
    "add",
    "Add two integers",
    &tool_inst, &test_tool::add,
    arg("a", "First integer"),
    arg("b", "Second integer")
  );

  registry.register_tool(
    "greet",
    "Greet someone",
    &tool_inst, &test_tool::greet,
    arg("name", "Name to greet")
  );

  // Test dispatch with add
  auto result = run_async_dispatch(registry, {
    {"function", {
      {"name", "add"},
      {"arguments", R"({"a": 5, "b": 3})"}
    }}
  });

  CHECK(result["result"] == 8);

  // Test dispatch with greet
  result = run_async_dispatch(registry, {
    {"function", {
      {"name", "greet"},
      {"arguments", R"({"name": "World"})"}
    }}
  });

  CHECK(result["result"] == "Hello, World");
}

TEST_CASE("tool_registry: dispatch unknown tool")
{
  using namespace mistral::agent;

  tool_registry registry;
  auto result = run_async_dispatch(registry, {
    {"function", {
      {"name", "nonexistent"},
      {"arguments", "{}"}
    }}
  });

  CHECK(result.contains("error"));
  CHECK(result["error"].get<std::string>().find("not found") != std::string::npos);
}

TEST_CASE("tool_registry: get_schema returns tool definitions")
{
  using namespace mistral::agent;

  tool_registry registry;
  test_tool tool_inst;

  registry.register_tool(
    "add",
    "Add two integers",
    &tool_inst, &test_tool::add,
    arg("a", "First integer"),
    arg("b", "Second integer")
  );

  auto schema = registry.get_schema();
  REQUIRE(schema.is_array());
  REQUIRE(schema.size() == 1);

  auto tool_def = schema[0];
  CHECK(tool_def["function"]["name"] == "add");
  CHECK(tool_def["function"]["description"] == "Add two integers");
  CHECK(tool_def["function"]["parameters"]["type"] == "object");

  auto props = tool_def["function"]["parameters"]["properties"];
  CHECK(props["a"]["type"] == "integer");
  CHECK(props["b"]["type"] == "integer");
  CHECK(props["a"]["description"] == "First integer");
  CHECK(props["b"]["description"] == "Second integer");
}

TEST_CASE("tool_registry: has_tool and get_tool")
{
  using namespace mistral::agent;

  tool_registry registry;
  test_tool tool_inst;

  registry.register_tool(
    "add",
    "Add two integers",
    &tool_inst, &test_tool::add,
    arg("a", "First integer"),
    arg("b", "Second integer")
  );

  CHECK(registry.has_tool("add") == true);
  CHECK(registry.has_tool("nonexistent") == false);

  auto tool = registry.get_tool("add");
  REQUIRE(tool != nullptr);

  auto tool2 = registry.get_tool("nonexistent");
  CHECK(tool2 == nullptr);
}

TEST_CASE("tool_registry: copy constructor creates independent copy")
{
  using namespace mistral::agent;

  tool_registry registry1;
  test_tool tool_inst;

  registry1.register_tool(
    "add",
    "Add two integers",
    &tool_inst, &test_tool::add,
    arg("a", "First integer"),
    arg("b", "Second integer")
  );

  tool_registry registry2 = registry1;

  CHECK(registry2.has_tool("add") == true);
  CHECK(registry1.has_tool("add") == true);

  // Verify they are different instances
  CHECK(&registry1 != &registry2);
}

TEST_CASE("tool_registry: get_tools_map")
{
  using namespace mistral::agent;

  tool_registry registry;
  test_tool tool_inst;

  registry.register_tool(
    "add",
    "Add two integers",
    &tool_inst, &test_tool::add,
    arg("a", "First integer"),
    arg("b", "Second integer")
  );

  registry.register_tool(
    "greet",
    "Greet someone",
    &tool_inst, &test_tool::greet,
    arg("name", "Name to greet")
  );

  const auto& tools_map = registry.get_tools_map();
  CHECK(tools_map.size() == 2);
  CHECK(tools_map.find("add") != tools_map.end());
  CHECK(tools_map.find("greet") != tools_map.end());
}

TEST_CASE("tool_registry: add_tool directly")
{
  using namespace mistral::agent;

  tool_registry registry;
  test_tool tool_inst;

  auto wrapper = std::make_shared<tool_wrapper<json, int, int>>(
    [&tool_inst](int a, int b) { return tool_inst.add(a, b); },
    std::vector<arg_info>{arg("a", "First"), arg("b", "Second")},
    "Add two integers"
  );

  registry.add_tool("manual_add", wrapper);

  CHECK(registry.has_tool("manual_add") == true);

  auto result = run_async_dispatch(registry, {
    {"function", {
      {"name", "manual_add"},
      {"arguments", R"({"a": 10, "b": 20})"}
    }}
  });

  CHECK(result["result"] == 30);
}
