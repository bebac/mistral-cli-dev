#include "mcp/mcp_tool.h"

#include <doctest/doctest.h>

#include <string>

namespace
{
  using json = mistral::mcp::json;
}

TEST_CASE("mcp_tool: a result within the cap is returned unchanged")
{
  json item = {{"type", "text"}, {"text", "hello"}};
  const json result = {{"content", json::array({item})}};

  const json capped = mistral::mcp::cap_result_size(result);

  CHECK(capped.is_object());
  CHECK(capped == result);
}

TEST_CASE("mcp_tool: a result exactly at the cap is returned unchanged")
{
  // Size the payload so the serialized result is exactly the cap.
  const std::size_t overhead = json{{"t", ""}}.dump().size();
  const json result = {{"t", std::string(mistral::mcp::k_max_result_bytes - overhead, 'x')}};

  REQUIRE(result.dump().size() == mistral::mcp::k_max_result_bytes);
  CHECK(mistral::mcp::cap_result_size(result).is_object());
}

TEST_CASE("mcp_tool: a result over the cap is truncated and flagged")
{
  json item = {{"type", "text"}, {"text", std::string(200 * 1024, 'x')}};
  const json result = {{"content", json::array({item})}};
  const std::string original = result.dump();

  const json capped = mistral::mcp::cap_result_size(result);

  REQUIRE(capped.is_string());
  const std::string text = capped.get<std::string>();
  // The kept prefix is exactly the first k_max_result_bytes of the result.
  CHECK(text.starts_with(original.substr(0, mistral::mcp::k_max_result_bytes)));
  // The marker names the total size and how much was dropped.
  CHECK(text.find("truncated") != std::string::npos);
  CHECK(text.find(std::to_string(original.size())) != std::string::npos);
  CHECK(text.find(std::to_string(original.size() - mistral::mcp::k_max_result_bytes)) != std::string::npos);
}
