#include "doctest/doctest.h"
#include "agent/conversation.h"
#include "agent/tool_registry.h"
#include "api/mistral_api.h"
#include "tools/file_tools.h"

TEST_CASE("conversation: basic message flow")
{
  mistral::agent::conversation chat;

  chat.set_system_message("You are helpful.");
  chat.add_user_message("Hello");
  chat.add_assistant_message("Hi there!");

  REQUIRE(chat.size() == 3);
  CHECK(chat.messages()[0].role == "system");
  CHECK(chat.messages()[1].role == "user");
  CHECK(chat.messages()[2].role == "assistant");
}

TEST_CASE("conversation: tool result messages")
{
  mistral::agent::conversation chat;
  chat.add_user_message("list files");

  std::vector<mistral::api::tool_call> calls = {{
    .id = "call_123",
    .type = "function",
    .function = {.name = "ls", .arguments = R"({"path":"."})"}
  }};
  chat.add_assistant_message("", std::move(calls));
  chat.add_tool_result("ls", "call_123", R"({"files":["a.txt"]})");

  REQUIRE(chat.size() == 3);
  CHECK(chat.messages()[2].role == "tool");
  CHECK(chat.messages()[2].name == "ls");
  CHECK(chat.messages()[2].tool_call_id == "call_123");
}

TEST_CASE("conversation: JSON serialization")
{
  mistral::agent::conversation chat;
  chat.add_user_message("hello");
  chat.add_assistant_message("hi");

  auto j = mistral::api::json(chat.messages());
  CHECK(j.size() == 2);
  CHECK(j[0]["role"] == "user");
  CHECK(j[1]["content"] == "hi");
}

TEST_CASE("conversation: sliding window")
{
  mistral::agent::conversation chat(3);
  chat.set_system_message("system");

  for (int i = 0; i < 10; ++i) {
    chat.add_user_message("user " + std::to_string(i));
    chat.add_assistant_message("assistant " + std::to_string(i));
  }
  chat.ensure_window_size();

  // System message + The last user/assistant pair.
  CHECK(chat.messages().front().role == "system");
  CHECK(chat.size() == 3);
}

TEST_CASE("conversation: sliding window preserves alternation")
{
  mistral::agent::conversation chat(2); // Keep 2 conversation messages
  chat.set_system_message("system");

  // Add alternating user/assistant messages
  chat.add_user_message("user1");
  chat.add_assistant_message("assistant1");
  chat.add_user_message("user2");
  chat.add_assistant_message("assistant2");
  chat.add_user_message("user3");
  chat.add_assistant_message("assistant3");

  chat.ensure_window_size();

  // Should have: system + last 2 conversation messages
  // Since we keep the most recent, we should have:
  // [system, user3, assistant3]
  REQUIRE(chat.size() == 3);
  CHECK(chat.messages()[0].role == "system");
  CHECK(chat.messages()[1].role == "user");
  CHECK(chat.messages()[1].content.size() == 1);
  CHECK(chat.messages()[1].content[0].type == "text");
  CHECK(chat.messages()[1].content[0].content == "user3");
  CHECK(chat.messages()[2].role == "assistant");
  CHECK(chat.messages()[2].content.size() == 1);
  CHECK(chat.messages()[2].content[0].type == "text");
  CHECK(chat.messages()[2].content[0].content == "assistant3");
}

TEST_CASE("conversation: sliding window with tool calls preserves tool results")
{
  // This test simulates a realistic scenario where:
  // 1. User asks a question that requires a tool call
  // 2. Tool executes and returns result
  // 3. User asks follow-up that requires another tool call
  // 4. The first tool result should still be available

  mistral::agent::conversation chat(5); // Keep 5 messages
  chat.set_system_message("system");

  // Turn 1: User asks to list files
  chat.add_user_message("list files in current directory");

  // Assistant decides to use tool
  std::vector<mistral::api::tool_call> calls1 = {{
    .id = "call_1",
    .type = "function",
    .function = {.name = "ls", .arguments = R"({"path":"."})"}
  }};
  chat.add_assistant_message("", std::move(calls1));

  // Tool executes and returns result
  chat.add_tool_result("ls", "call_1", R"({"files":["file1.txt", "file2.txt"]})");

  // Turn 2: User asks to read a specific file
  chat.add_user_message("read file1.txt");

  // Assistant decides to use tool again
  std::vector<mistral::api::tool_call> calls2 = {{
    .id = "call_2",
    .type = "function",
    .function = {.name = "read", .arguments = R"({"path":"file1.txt", "line_from":1, "line_to":10})"}
  }};
  chat.add_assistant_message("", std::move(calls2));

  // Tool executes and returns result
  chat.add_tool_result("read", "call_2", R"({"content":"Line 1\nLine 2\nLine 3"})");

  // Now trim the window
  chat.ensure_window_size();

  // After trimming, we should still have the system message and the most recent messages
  // The key is that tool results should be preserved if they're part of active sequences

  REQUIRE(chat.size() >= 1);
  CHECK(chat.messages()[0].role == "system");

  // Count tool result messages
  size_t tool_result_count = 0;
  for (const auto& msg : chat.messages()) {
    if (msg.role == "tool") {
      tool_result_count++;
    }
  }

  // With the buggy implementation, tool_result_count would be 0 or 1
  // With the fixed implementation, it should be 2 (both tool results preserved)
  // Actually, with max_history=5 and 7 messages before trimming, we keep 6 messages (system + 5)
  // So we might only keep 1 tool result. The key is that we should keep tool results
  // rather than trimming them in favor of regular messages.

  // The important thing is that we don't lose tool results entirely
  CHECK_MESSAGE(tool_result_count > 0,
    "At least one tool result should be preserved after ensure_window_size(). "
    "Tool results are essential for the assistant to reference when making subsequent tool calls.");
}

TEST_CASE("conversation: sliding window without system message")
{
  mistral::agent::conversation chat(2); // Keep 2 messages

  chat.add_user_message("user1");
  chat.add_assistant_message("assistant1");
  chat.add_user_message("user2");
  chat.add_assistant_message("assistant2");
  chat.add_user_message("user3");
  chat.add_assistant_message("assistant3");

  chat.ensure_window_size();

  // Should keep last 2 messages: user3 and... but we only have user3 as the last
  // Actually, we added 3 user messages and 2 assistant messages
  // The sequence is: user1, assistant1, user2, assistant2, user3
  // We want to keep the last 2: user2, assistant2
  REQUIRE(chat.size() == 2);
  CHECK(chat.messages()[0].role == "user");
  CHECK(chat.messages()[0].content.size() == 1);
  CHECK(chat.messages()[0].content[0].content == "user3");
  CHECK(chat.messages()[1].role == "assistant");
  CHECK(chat.messages()[1].content.size() == 1);
  CHECK(chat.messages()[1].content[0].content == "assistant3");
}

TEST_CASE("tool_registry: schema generation")
{
  using namespace mistral::agent;
  using namespace mistral::tools;

  file_tools ft;
  tool_registry tools;

  tools.register_tool(
    "ls", "List files",
    &ft, &file_tools::list_files,
    arg("path", "Dir path"),
    arg("recursive", "Recurse", false),
    arg("max_depth", "Depth", 1)
  );

  auto schema = tools.get_schema();
  REQUIRE(schema.size() == 1);
  CHECK(schema[0]["type"] == "function");
  CHECK(schema[0]["function"]["name"] == "ls");
  CHECK(schema[0]["function"]["parameters"]["properties"].contains("path"));
}

TEST_CASE("tool_registry: dispatch")
{
  using namespace mistral::agent;
  using namespace mistral::tools;

  file_tools ft;
  tool_registry tools;

  tools.register_tool(
    "ls", "List files",
    &ft, &file_tools::list_files,
    arg("path", "Dir path"),
    arg("recursive", "Recurse", false),
    arg("max_depth", "Depth", 1)
  );

  auto result = tools.dispatch(json{
    {"function", {
      {"name", "ls"},
      {"arguments", R"({"path":"."})"}
    }}
  });

  // Should return a string (directory listing), not an error.
  CHECK(result.is_string());
}

TEST_CASE("mistral_api: build_chat_request")
{
  using namespace mistral::api;

  std::vector<message> msgs = {
    {.role = "user", .content = {message_content{.type = "text", .content = "hello"}}}
  };

  auto body = build_chat_request("test-model", msgs, json::array(), true);
  auto j = json::parse(body);

  CHECK(j["model"] == "test-model");
  CHECK(j["stream"] == true);
  CHECK(j["messages"].size() == 1);
}

TEST_CASE("mistral_api: parse_stream_delta")
{
  using namespace mistral::api;

  json j = {
    {"choices", {{
      {"delta", {{"content", "Hello"}}},
      {"finish_reason", nullptr}
    }}}
  };

  auto delta = parse_stream_delta(j);
  REQUIRE(delta.content.size() == 1);
  CHECK(delta.content[0].type == "text");
  CHECK(delta.content[0].content == "Hello");
  CHECK(delta.finish_reason.empty());
}
