#include "agent/approval_manager.h"
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

namespace mistral::agent
{
  using json = nlohmann::ordered_json;

  TEST_SUITE_BEGIN("approval_manager");

  // ---- Test Fixture ----

  struct approval_manager_fixture
  {
    approval_manager manager;
  };

  // ---- Basic Construction ----

  TEST_CASE_FIXTURE(approval_manager_fixture, "construction")
  {
    // Should be able to construct without errors
    CHECK_NOTHROW(approval_manager());
    CHECK(manager.get_session_approvals().empty());
  }

  // ---- Session Approval Management ----

  TEST_CASE_FIXTURE(approval_manager_fixture, "add_session_approval")
  {
    manager.add_session_approval("write", {{"path", "/tmp/test.txt"}});

    const auto& approvals = manager.get_session_approvals();
    CHECK(approvals.size() == 1);
    CHECK(approvals[0].tool_name == "write");
    CHECK(approvals[0].argument_pattern["path"] == "/tmp/test.txt");
  }

  TEST_CASE_FIXTURE(approval_manager_fixture, "clear_session_approvals")
  {
    manager.add_session_approval("write", {{"path", "/tmp/test.txt"}});
    manager.add_session_approval("read", {{"path", "/tmp/test.txt"}});

    CHECK(manager.get_session_approvals().size() == 2);

    manager.clear_session_approvals();
    CHECK(manager.get_session_approvals().empty());
  }

  // ---- is_session_approved Tests ----

  TEST_CASE_FIXTURE(approval_manager_fixture, "is_session_approved_exact_match")
  {
    manager.add_session_approval("write", {{"path", "/tmp/test.txt"}});

    json args1 = {{"path", "/tmp/test.txt"}};
    json args2 = {{"path", "/tmp/other.txt"}};
    json args3 = {{"path", "/tmp/test.txt"}, {"content", "hello"}};

    CHECK(manager.is_session_approved("write", args1));
    CHECK(!manager.is_session_approved("write", args2));
    // args3 has extra field but path matches
    CHECK(manager.is_session_approved("write", args3));
  }

  TEST_CASE_FIXTURE(approval_manager_fixture, "is_session_approved_tool_mismatch")
  {
    manager.add_session_approval("write", {{"path", "/tmp/test.txt"}});

    json args = {{"path", "/tmp/test.txt"}};

    CHECK(!manager.is_session_approved("read", args));
  }

  TEST_CASE_FIXTURE(approval_manager_fixture, "is_session_approved_empty_pattern")
  {
    // Empty pattern should match any arguments for the tool
    manager.add_session_approval("write", json{});

    json args1 = {{"path", "/tmp/test.txt"}};
    json args2 = {{"path", "/etc/passwd"}};
    json args3 = {{"content", "hello"}};

    CHECK(manager.is_session_approved("write", args1));
    CHECK(manager.is_session_approved("write", args2));
    CHECK(manager.is_session_approved("write", args3));
  }

  TEST_CASE_FIXTURE(approval_manager_fixture, "is_session_approved_multiple_fields")
  {
    manager.add_session_approval("write", {{"path", "/tmp/test.txt"}, {"content", "hello"}});

    json args_match = {{"path", "/tmp/test.txt"}, {"content", "hello"}};
    json args_path_mismatch = {{"path", "/tmp/other.txt"}, {"content", "hello"}};
    json args_content_mismatch = {{"path", "/tmp/test.txt"}, {"content", "world"}};
    json args_extra_field = {{"path", "/tmp/test.txt"}, {"content", "hello"}, {"mode", "append"}};

    CHECK(manager.is_session_approved("write", args_match));
    CHECK(!manager.is_session_approved("write", args_path_mismatch));
    CHECK(!manager.is_session_approved("write", args_content_mismatch));
    CHECK(manager.is_session_approved("write", args_extra_field));
  }

  // ---- Path Regex Matching ----

  TEST_CASE_FIXTURE(approval_manager_fixture, "path_regex_matching_directory")
  {
    // Add approval for a directory path (ending with /)
    manager.add_session_approval("write", {{"path", "/tmp/"}});

    json args_in_tmp = {{"path", "/tmp/test.txt"}};
    json args_in_tmp_sub = {{"path", "/tmp/sub/file.txt"}};
    json args_outside = {{"path", "/var/test.txt"}};

    CHECK(manager.is_session_approved("write", args_in_tmp));
    CHECK(manager.is_session_approved("write", args_in_tmp_sub));
    CHECK(!manager.is_session_approved("write", args_outside));
  }

  TEST_CASE_FIXTURE(approval_manager_fixture, "path_regex_matching_exact_file")
  {
    manager.add_session_approval("write", {{"path", "/tmp/test.txt"}});

    json args_exact = {{"path", "/tmp/test.txt"}};
    json args_other = {{"path", "/tmp/other.txt"}};

    CHECK(manager.is_session_approved("write", args_exact));
    CHECK(!manager.is_session_approved("write", args_other));
  }

  // ---- Multiple Session Approvals ----

  TEST_CASE_FIXTURE(approval_manager_fixture, "multiple_session_approvals")
  {
    manager.add_session_approval("write", {{"path", "/tmp/"}});
    manager.add_session_approval("read", {{"path", "/home/"}});

    json write_args = {{"path", "/tmp/test.txt"}};
    json read_args = {{"path", "/home/user/file.txt"}};

    CHECK(manager.is_session_approved("write", write_args));
    CHECK(manager.is_session_approved("read", read_args));
    CHECK(!manager.is_session_approved("write", read_args));
    CHECK(!manager.is_session_approved("read", write_args));
  }

  TEST_SUITE_END();
} // namespace mistral::agent
