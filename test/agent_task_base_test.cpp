#include "agent/agent_task_base.h"
#include "agent/agent_system.h"
#include "agent/edit_parser.h"
#include "api/types.h"
#include "doctest/doctest.h"

#include <string>
#include <vector>

namespace mistral::agent::test
{

TEST_SUITE_BEGIN("agent_task_base_patchset_parsing");

TEST_CASE("patchset parsing - patchset with tool call")
{
    // This test verifies that when an AI response contains both
    // a patchset and a tool call, the patchset is stored before
    // the tool is executed.

    // Note: This is an integration test that requires mocking or
    // a more complex setup. For now, we test the parsing logic directly.

    // Simulate accumulated text content with patchset
    std::string accumulated_text =
        "Here are the changes (patchset: test-patchset):\n"
        "\n"
        "src/main.cpp\n"
        "~~~cpp\n"
        "<<<<<<< SEARCH\n"
        "old_code\n"
        "=======\n"
        "new_code\n"
        ">>>>>>> REPLACE\n"
        "~~~\n"
        "\n"
        "patchset_validate(patchset_id=\"test-patchset\")";

    // Parse and store
    auto parse_result = parse_edits_with_patchset(accumulated_text);
    REQUIRE(parse_result.has_value());
    CHECK(parse_result->first == "test-patchset");
    CHECK(parse_result->second.size() == 1);
    CHECK(parse_result->second[0].patch_number == 1);

    // Store in agent_system
    auto& agent_sys = agent_system::instance();
    agent_sys.remove_patchset("test-patchset"); // Clear first
    agent_sys.store_patchset(parse_result->first, parse_result->second);

    // Verify it's stored
    CHECK(agent_sys.has_patchset());
    auto current_id = agent_sys.get_current_patchset_id();
    CHECK(current_id.has_value());
    CHECK(*current_id == "test-patchset");

    // Verify we can retrieve it
    auto retrieved = agent_sys.get_patchset("test-patchset");
    REQUIRE(retrieved.has_value());
    CHECK(retrieved->edits[0].file_path == "src/main.cpp");

    // Clean up
    agent_sys.remove_patchset("test-patchset");
}

TEST_CASE("patchset parsing - multiple edits with tool call")
{
    std::string accumulated_text =
        "Here are the changes (patchset: multi-edit):\n"
        "\n"
        "src/main.cpp\n"
        "~~~cpp\n"
        "<<<<<<< SEARCH\n"
        "#include <iostream>\n"
        "=======\n"
        "#include <iostream>\n"
        "#include <vector>\n"
        ">>>>>>> REPLACE\n"
        "~~~\n"
        "\n"
        "src/utils.cpp\n"
        "~~~cpp\n"
        "<<<<<<< SEARCH\n"
        "helper()\n"
        "=======\n"
        "fixed_helper()\n"
        ">>>>>>> REPLACE\n"
        "~~~\n"
        "\n"
        "patchset_apply(patchset_id=\"multi-edit\")";

    auto parse_result = parse_edits_with_patchset(accumulated_text);
    REQUIRE(parse_result.has_value());
    CHECK(parse_result->first == "multi-edit");
    CHECK(parse_result->second.size() == 2);
    CHECK(parse_result->second[0].patch_number == 1);
    CHECK(parse_result->second[1].patch_number == 2);

    // Store and verify
    auto& agent_sys = agent_system::instance();
    agent_sys.remove_patchset("multi-edit");
    agent_sys.store_patchset(parse_result->first, parse_result->second);

    auto retrieved = agent_sys.get_patchset("multi-edit");
    REQUIRE(retrieved.has_value());
    CHECK(retrieved->edits.size() == 2);
    CHECK(retrieved->edits[0].file_path == "src/main.cpp");
    CHECK(retrieved->edits[1].file_path == "src/utils.cpp");

    // Clean up
    agent_sys.remove_patchset("multi-edit");
}

TEST_CASE("patchset parsing - no patchset in response")
{
    std::string accumulated_text =
        "This is just a regular response without any patchset.\n"
        "Here's some information for the user.\n"
        "read(path=\"src/main.cpp\")";

    auto parse_result = parse_edits_with_patchset(accumulated_text);
    CHECK(!parse_result.has_value());

    // agent_system should not have a patchset
    auto& agent_sys = agent_system::instance();
    agent_sys.remove_patchset("no-patchset");
    CHECK(!agent_sys.has_patchset());
}

TEST_CASE("patchset parsing - patchset without tool call")
{
    // When there's a patchset but no tool calls (final answer)
    std::string accumulated_text =
        "Here are the changes (patchset: final-answer):\n"
        "\n"
        "src/main.cpp\n"
        "~~~cpp\n"
        "<<<<<<< SEARCH\n"
        "old\n"
        "=======\n"
        "new\n"
        ">>>>>>> REPLACE\n"
        "~~~";

    auto parse_result = parse_edits_with_patchset(accumulated_text);
    REQUIRE(parse_result.has_value());
    CHECK(parse_result->first == "final-answer");

    // Store and verify
    auto& agent_sys = agent_system::instance();
    agent_sys.remove_patchset("final-answer");
    agent_sys.store_patchset(parse_result->first, parse_result->second);

    CHECK(agent_sys.has_patchset());
    CHECK(agent_sys.get_current_patchset_id() == "final-answer");

    // Clean up
    agent_sys.remove_patchset("final-answer");
}

TEST_SUITE_END;

// ===========================================================================
// is_retryable_error tests
// ===========================================================================

TEST_SUITE_BEGIN("agent_task_base_retryable_error");

TEST_CASE("HTTP 503 is retryable via http_status")
{
  api::api_event evt{
    api::event_type::error,
    "HTTP 503: Service Unavailable",
    503
  };
  CHECK(agent_task_base::is_retryable_error(evt));
}

TEST_CASE("HTTP 502 is retryable via http_status")
{
  api::api_event evt{
    api::event_type::error,
    "HTTP 502: Bad Gateway",
    502
  };
  CHECK(agent_task_base::is_retryable_error(evt));
}

TEST_CASE("HTTP 429 is retryable via http_status")
{
  api::api_event evt{
    api::event_type::error,
    "HTTP 429: Too Many Requests",
    429
  };
  CHECK(agent_task_base::is_retryable_error(evt));
}

TEST_CASE("HTTP 500 is not retryable via http_status")
{
  api::api_event evt{
    api::event_type::error,
    "HTTP 500: Internal Server Error",
    500
  };
  CHECK(!agent_task_base::is_retryable_error(evt));
}

TEST_CASE("HTTP 404 is not retryable via http_status")
{
  api::api_event evt{
    api::event_type::error,
    "HTTP 404: Not Found",
    404
  };
  CHECK(!agent_task_base::is_retryable_error(evt));
}

TEST_CASE("HTTP 401 is not retryable via http_status")
{
  api::api_event evt{
    api::event_type::error,
    "HTTP 401: Unauthorized",
    401
  };
  CHECK(!agent_task_base::is_retryable_error(evt));
}

TEST_CASE("HTTP 200 is not retryable via http_status")
{
  api::api_event evt{
    api::event_type::error,
    "HTTP 200: OK",
    200
  };
  CHECK(!agent_task_base::is_retryable_error(evt));
}

TEST_CASE("connection closed is retryable without http_status")
{
  api::api_event evt{
    api::event_type::error,
    "connection closed"
  };
  CHECK(agent_task_base::is_retryable_error(evt));
}

TEST_CASE("connection error is retryable without http_status")
{
  api::api_event evt{
    api::event_type::error,
    "connection error: Connection refused"
  };
  CHECK(agent_task_base::is_retryable_error(evt));
}

TEST_CASE("stream reset is retryable without http_status")
{
  api::api_event evt{
    api::event_type::error,
    "stream reset: INTERNAL_ERROR"
  };
  CHECK(agent_task_base::is_retryable_error(evt));
}

TEST_CASE("send error is retryable without http_status")
{
  api::api_event evt{
    api::event_type::error,
    "send error: Broken pipe"
  };
  CHECK(agent_task_base::is_retryable_error(evt));
}

TEST_CASE("malformed SSE data is not retryable")
{
  api::api_event evt{
    api::event_type::error,
    "malformed SSE data '{}' unexpected token"
  };
  CHECK(!agent_task_base::is_retryable_error(evt));
}

TEST_CASE("generic error message is not retryable")
{
  api::api_event evt{
    api::event_type::error,
    "got finish_reason 'error'"
  };
  CHECK(!agent_task_base::is_retryable_error(evt));
}

TEST_CASE("empty error payload is not retryable")
{
  api::api_event evt{
    api::event_type::error,
    ""
  };
  CHECK(!agent_task_base::is_retryable_error(evt));
}

TEST_CASE("http_status takes precedence over string matching")
{
  // Even though the payload contains "connection closed", the http_status
  // field should take precedence. A 500 status code is not retryable.
  api::api_event evt{
    api::event_type::error,
    "HTTP 500: connection closed by server",
    500
  };
  CHECK(!agent_task_base::is_retryable_error(evt));
}

TEST_CASE("http_status 503 with connection closed in payload is retryable")
{
  // The http_status field takes precedence and 503 is retryable.
  api::api_event evt{
    api::event_type::error,
    "HTTP 503: connection closed by server",
    503
  };
  CHECK(agent_task_base::is_retryable_error(evt));
}

TEST_CASE("default-constructed api_event is not retryable")
{
  api::api_event evt{};
  CHECK(!agent_task_base::is_retryable_error(evt));
}

TEST_SUITE_END;

} // namespace mistral::agent::test