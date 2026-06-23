#include "agent/approval_prompt.h"
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

namespace mistral::agent
{
  using json = nlohmann::ordered_json;

  TEST_SUITE_BEGIN("approval_prompt");

  // ---- parse_approval_response Tests ----

  TEST_CASE("parse_approval_response handles empty input")
  {
    auto result = parse_approval_response("");
    CHECK(result == approval_action::REJECT);
  }

  TEST_CASE("parse_approval_response handles approve")
  {
    CHECK(parse_approval_response("y") == approval_action::APPROVE);
    CHECK(parse_approval_response("Y") == approval_action::APPROVE);
    CHECK(parse_approval_response("yes") == approval_action::APPROVE);
  }

  TEST_CASE("parse_approval_response handles reject")
  {
    CHECK(parse_approval_response("n") == approval_action::REJECT);
    CHECK(parse_approval_response("N") == approval_action::REJECT);
    CHECK(parse_approval_response("no") == approval_action::REJECT);
  }

  TEST_CASE("parse_approval_response handles reject with message")
  {
    CHECK(parse_approval_response("r") == approval_action::REJECT_WITH_MESSAGE);
    CHECK(parse_approval_response("R") == approval_action::REJECT_WITH_MESSAGE);
  }

  TEST_CASE("parse_approval_response handles approve all")
  {
    CHECK(parse_approval_response("a") == approval_action::APPROVE_ALL);
    CHECK(parse_approval_response("A") == approval_action::APPROVE_ALL);
  }

  TEST_CASE("parse_approval_response handles help request")
  {
    auto result = parse_approval_response("?");
    CHECK(!result.has_value());
  }

  TEST_CASE("parse_approval_response handles unknown input")
  {
    CHECK(parse_approval_response("x") == approval_action::REJECT);
    CHECK(parse_approval_response("z") == approval_action::REJECT);
    CHECK(parse_approval_response("random") == approval_action::REJECT_WITH_MESSAGE); // 'r' maps to REJECT_WITH_MESSAGE
    CHECK(parse_approval_response("q") == approval_action::REJECT);
  }

  // ---- build_approval_prompt Tests ----

  TEST_CASE("build_approval_prompt includes tool name")
  {
    std::string prompt = build_approval_prompt("test_tool", json::object(), "test reason");
    CHECK(prompt.find("test_tool") != std::string::npos);
    CHECK(prompt.find("[APPROVAL REQUIRED]") != std::string::npos);
  }

  TEST_CASE("build_approval_prompt includes reason")
  {
    std::string prompt = build_approval_prompt("test_tool", json::object(), "test reason");
    CHECK(prompt.find("test reason") != std::string::npos);
  }

  TEST_CASE("build_approval_prompt includes arguments when present")
  {
    json args;
    args["arg1"] = "value1";
    args["arg2"] = 42;
    std::string prompt = build_approval_prompt("test_tool", args, "test reason");
    CHECK(prompt.find("Arguments:") != std::string::npos);
    CHECK(prompt.find("arg1") != std::string::npos);
    CHECK(prompt.find("value1") != std::string::npos);
    CHECK(prompt.find("arg2") != std::string::npos);
    CHECK(prompt.find("42") != std::string::npos);
  }

  TEST_CASE("build_approval_prompt handles empty arguments")
  {
    std::string prompt = build_approval_prompt("test_tool", json::object(), "test reason");
    CHECK(prompt.find("Arguments:") == std::string::npos);
  }

  TEST_CASE("build_approval_prompt truncates long strings")
  {
    std::string long_value(100, 'x');
    json args = {
      {"long_arg", long_value}
    };
    std::string prompt = build_approval_prompt("test_tool", args, "test reason");
    CHECK(prompt.find("...") != std::string::npos);
    CHECK(prompt.find(long_value) == std::string::npos);
  }

  TEST_CASE("build_approval_prompt handles boolean values")
  {
    json args = {
      {"bool_true", true},
      {"bool_false", false}
    };
    std::string prompt = build_approval_prompt("test_tool", args, "test reason");
    CHECK(prompt.find("true") != std::string::npos);
    CHECK(prompt.find("false") != std::string::npos);
  }

  TEST_CASE("build_approval_prompt handles numeric values")
  {
    json args = {
      {"int_val", 42},
      {"double_val", 3.14}
    };
    std::string prompt = build_approval_prompt("test_tool", args, "test reason");
    CHECK(prompt.find("42") != std::string::npos);
    CHECK(prompt.find("3.14") != std::string::npos);
  }

  TEST_CASE("build_approval_prompt includes approval options")
  {
    std::string prompt = build_approval_prompt("test_tool", json::object(), "test reason");
    CHECK(prompt.find("[y/n/r/a/?]") != std::string::npos);
  }

  // ---- get_approval_help_message Tests ----

  TEST_CASE("get_approval_help_message contains all options")
  {
    std::string help = get_approval_help_message();
    CHECK(help.find("y - Approve this specific call") != std::string::npos);
    CHECK(help.find("n - Reject this call") != std::string::npos);
    CHECK(help.find("r - Reject with custom message for AI") != std::string::npos);
    CHECK(help.find("a - Approve all similar calls for this session") != std::string::npos);
    CHECK(help.find("? - Show this help") != std::string::npos);
    CHECK(help.find("[y/n/r/a/?]") != std::string::npos);
  }

  TEST_SUITE_END();

} // namespace mistral::agent
