#include "agent/tool_registry.h"
#include <doctest/doctest.h>

#include <dripcore/scheduler.h>
#include <dripcore/task.h>

#include <memory>
#include <string>

namespace mistral::agent
{
  // Test tool class - methods return json objects
  class test_tool
  {
  public:
    json add(int a, int b)
    {
      return {{"result", a + b}};
    }
    json greet(std::string name)
    {
      return {{"result", "Hello, " + name}};
    }
    json multiply(int a, int b)
    {
      return {{"result", a * b}};
    }
    json greet_titled(std::string name, std::string title)
    {
      return {{"result", "Hello, " + title + " " + name}};
    }
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
        : registry_(reg), tool_call_(std::move(tc)), out_result_(out_result), out_completed_(out_completed)
      {
      }

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

    if (completed)
    {
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
    "add", "Add two integers", &tool_inst, &test_tool::add, arg("a", "First integer"), arg("b", "Second integer")
  );

  registry.register_tool("greet", "Greet someone", &tool_inst, &test_tool::greet, arg("name", "Name to greet"));

  // Test dispatch with add
  auto result = run_async_dispatch(registry, {{"function", {{"name", "add"}, {"arguments", R"({"a": 5, "b": 3})"}}}});

  CHECK(result["result"] == 8);

  // Test dispatch with greet
  result = run_async_dispatch(registry, {{"function", {{"name", "greet"}, {"arguments", R"({"name": "World"})"}}}});

  CHECK(result["result"] == "Hello, World");
}

TEST_CASE("tool_registry: dispatch unknown tool")
{
  using namespace mistral::agent;

  tool_registry registry;
  auto result = run_async_dispatch(registry, {{"function", {{"name", "nonexistent"}, {"arguments", "{}"}}}});

  CHECK(result.is_string());
  CHECK(result.get<std::string>().find("not found") != std::string::npos);
}

TEST_CASE("tool_registry: dispatch with wrong argument type")
{
  using namespace mistral::agent;

  tool_registry registry;
  test_tool tool_inst;

  registry.register_tool(
    "add", "Add two integers", &tool_inst, &test_tool::add, arg("a", "First integer"), arg("b", "Second integer")
  );

  // Pass a string where an integer is expected
  auto result =
    run_async_dispatch(registry, {{"function", {{"name", "add"}, {"arguments", R"({"a": "hello", "b": 3})"}}}});

  REQUIRE(result.is_string());
  auto msg = result.get<std::string>();

  // The dispatch catch block prepends "Error: "
  CHECK(msg.starts_with("Error: "));
  // The enhanced message should include the argument name
  CHECK(msg.find("'a'") != std::string::npos);
  // Should mention the expected type
  CHECK(msg.find("expected integer") != std::string::npos);
  // Should mention the actual type received
  CHECK(msg.find("received string") != std::string::npos);
  // Should include the offending value
  CHECK(msg.find("\"hello\"") != std::string::npos);
}

TEST_CASE("tool_registry: dispatch with wrong argument type for string param")
{
  using namespace mistral::agent;

  tool_registry registry;
  test_tool tool_inst;

  registry.register_tool("greet", "Greet someone", &tool_inst, &test_tool::greet, arg("name", "Name to greet"));

  // Pass an integer where a string is expected
  auto result = run_async_dispatch(registry, {{"function", {{"name", "greet"}, {"arguments", R"({"name": 42})"}}}});

  REQUIRE(result.is_string());
  auto msg = result.get<std::string>();

  CHECK(msg.starts_with("Error: "));
  CHECK(msg.find("'name'") != std::string::npos);
  CHECK(msg.find("expected string") != std::string::npos);
  CHECK(msg.find("received integer") != std::string::npos);
  CHECK(msg.find("42") != std::string::npos);
}

TEST_CASE("tool_registry: get_schema returns tool definitions")
{
  using namespace mistral::agent;

  tool_registry registry;
  test_tool tool_inst;

  registry.register_tool(
    "add", "Add two integers", &tool_inst, &test_tool::add, arg("a", "First integer"), arg("b", "Second integer")
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
    "add", "Add two integers", &tool_inst, &test_tool::add, arg("a", "First integer"), arg("b", "Second integer")
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
    "add", "Add two integers", &tool_inst, &test_tool::add, arg("a", "First integer"), arg("b", "Second integer")
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
    "add", "Add two integers", &tool_inst, &test_tool::add, arg("a", "First integer"), arg("b", "Second integer")
  );

  registry.register_tool("greet", "Greet someone", &tool_inst, &test_tool::greet, arg("name", "Name to greet"));

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

  auto result =
    run_async_dispatch(registry, {{"function", {{"name", "manual_add"}, {"arguments", R"({"a": 10, "b": 20})"}}}});

  CHECK(result["result"] == 30);
}

TEST_CASE("tool_registry: strict mode off by default")
{
  using namespace mistral::agent;

  tool_registry registry;
  test_tool tool_inst;

  registry.register_tool(
    "greet_titled",
    "Greet someone with a title",
    &tool_inst,
    &test_tool::greet_titled,
    arg("name", "Name to greet"),
    arg("title", "Optional title", std::string("Mr."))
  );

  CHECK(registry.strict_mode() == false);

  auto schema = registry.get_schema();
  REQUIRE(schema.is_array());
  REQUIRE(schema.size() == 1);

  auto tool_def = schema[0];

  // No "strict" field in the function object
  CHECK(tool_def["function"].contains("strict") == false);

  // No "additionalProperties" in parameters
  CHECK(tool_def["function"]["parameters"].contains("additionalProperties") == false);

  // Only required args (non-default) are in "required"
  auto required = tool_def["function"]["parameters"]["required"];
  CHECK(required == json::array({"name"}));

  // Optional arg (with default) is still present in properties
  auto props = tool_def["function"]["parameters"]["properties"];
  CHECK(props.contains("name") == true);
  CHECK(props.contains("title") == true);
  CHECK(props["title"]["default"] == "Mr.");
}

TEST_CASE("tool_registry: strict mode on includes strict and additionalProperties")
{
  using namespace mistral::agent;

  tool_registry registry;
  test_tool tool_inst;

  registry.register_tool(
    "greet_titled",
    "Greet someone with a title",
    &tool_inst,
    &test_tool::greet_titled,
    arg("name", "Name to greet"),
    arg("title", "Optional title", std::string("Mr."))
  );

  registry.set_strict_mode(true);
  CHECK(registry.strict_mode() == true);

  auto schema = registry.get_schema();
  REQUIRE(schema.is_array());
  REQUIRE(schema.size() == 1);

  auto tool_def = schema[0];

  // "strict": true inside the function object
  CHECK(tool_def["function"]["strict"] == true);

  // "additionalProperties": false on parameters
  CHECK(tool_def["function"]["parameters"]["additionalProperties"] == false);

  // ALL property names (including those with defaults) are in "required"
  auto required = tool_def["function"]["parameters"]["required"];
  CHECK(required.size() == 2);
  CHECK(required[0] == "name");
  CHECK(required[1] == "title");
}

TEST_CASE("tool_registry: strict mode does not affect argument parsing")
{
  using namespace mistral::agent;

  tool_registry registry;
  test_tool tool_inst;

  registry.register_tool(
    "greet_titled",
    "Greet someone with a title",
    &tool_inst,
    &test_tool::greet_titled,
    arg("name", "Name to greet"),
    arg("title", "Optional title", std::string("Mr."))
  );

  registry.set_strict_mode(true);

  // Dispatch omitting the optional arg — default value must still be used
  auto result =
    run_async_dispatch(registry, {{"function", {{"name", "greet_titled"}, {"arguments", R"({"name": "World"})"}}}});

  CHECK(result["result"] == "Hello, Mr. World");

  // Dispatch providing the optional arg explicitly
  result = run_async_dispatch(
    registry, {{"function", {{"name", "greet_titled"}, {"arguments", R"({"name": "World", "title": "Dr."})"}}}}
  );

  CHECK(result["result"] == "Hello, Dr. World");
}

TEST_CASE("tool_registry: strict mode is per-registry, not global")
{
  using namespace mistral::agent;

  test_tool tool_inst;

  tool_registry strict_registry;
  strict_registry.register_tool(
    "greet_titled",
    "Greet someone with a title",
    &tool_inst,
    &test_tool::greet_titled,
    arg("name", "Name to greet"),
    arg("title", "Optional title", std::string("Mr."))
  );
  strict_registry.set_strict_mode(true);

  tool_registry plain_registry;
  plain_registry.register_tool(
    "greet_titled",
    "Greet someone with a title",
    &tool_inst,
    &test_tool::greet_titled,
    arg("name", "Name to greet"),
    arg("title", "Optional title", std::string("Mr."))
  );

  auto strict_schema = strict_registry.get_schema();
  auto plain_schema = plain_registry.get_schema();

  // Schemas differ: one has strict, the other doesn't
  CHECK(strict_schema != plain_schema);
  CHECK(strict_schema[0]["function"]["strict"] == true);
  CHECK(plain_schema[0]["function"].contains("strict") == false);

  // Copies preserve the strict mode setting
  tool_registry copy = strict_registry;
  CHECK(copy.strict_mode() == true);
  CHECK(copy.get_schema() == strict_schema);
}
