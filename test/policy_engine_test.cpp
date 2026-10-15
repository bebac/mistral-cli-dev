#include <doctest/doctest.h>
#include "agent/policy_engine.h"
#include "mcp/mcp_manager.h"

#include <string>
#include <vector>
#include <algorithm>

namespace mistral::agent
{
  using json = nlohmann::ordered_json;

  // Helper function for tests
  inline bool string_ends_with(const std::string& s, const std::string& suffix)
  {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
  }
}

TEST_SUITE_BEGIN("policy_engine");

// ---- Basic Policy Engine Tests ----

TEST_CASE("Default policy is DENY")
{
  mistral::agent::policy_engine engine;
  CHECK(engine.get_default_policy() == mistral::agent::policy_decision::DENY);
}

TEST_CASE("Set default policy")
{
  mistral::agent::policy_engine engine;
  engine.set_default_policy(mistral::agent::policy_decision::ALLOW);
  CHECK(engine.get_default_policy() == mistral::agent::policy_decision::ALLOW);

  engine.set_default_policy(mistral::agent::policy_decision::REQUIRE_APPROVAL);
  CHECK(engine.get_default_policy() == mistral::agent::policy_decision::REQUIRE_APPROVAL);
}

TEST_CASE("Empty engine with DENY default denies all")
{
  mistral::agent::policy_engine engine;
  mistral::agent::policy_context ctx{
    .tool_name = "ls",
    .tool_arguments = mistral::agent::json{
      {"path", "/tmp"}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::DENY);
}

TEST_CASE("Empty engine with ALLOW default allows all")
{
  mistral::agent::policy_engine engine;
  engine.set_default_policy(mistral::agent::policy_decision::ALLOW);

  mistral::agent::policy_context ctx{
    .tool_name = "ls",
    .tool_arguments = mistral::agent::json{
      {"path", "/tmp"}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

// ---- Tool-Based Rules ----

TEST_CASE("Allow specific tool")
{
  mistral::agent::policy_engine engine;
  engine.allow_tool("ls");

  mistral::agent::policy_context ctx{
    .tool_name = "ls",
    .tool_arguments = mistral::agent::json{
      {"path", "/tmp"}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
  CHECK(result.reason.has_value());
}

TEST_CASE("Allow tool does not allow different tool")
{
  mistral::agent::policy_engine engine;
  engine.allow_tool("ls");

  mistral::agent::policy_context ctx{
    .tool_name = "read",
    .tool_arguments = mistral::agent::json{
      {"path", "/tmp/file.txt"}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::DENY);
}

TEST_CASE("Require approval for specific tool")
{
  mistral::agent::policy_engine engine;
  engine.require_approval_tool("write");

  mistral::agent::policy_context ctx{
    .tool_name = "write",
    .tool_arguments = mistral::agent::json{
      {"path", "/tmp/file.txt"},
      {"content", "hello"}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::REQUIRE_APPROVAL);
}

TEST_CASE("Allow overrides require approval for same tool")
{
  mistral::agent::policy_engine engine;
  engine.require_approval_tool("write");
  engine.allow_tool("write");

  mistral::agent::policy_context ctx{
    .tool_name = "write",
    .tool_arguments = mistral::agent::json{
      {"path", "/tmp/file.txt"},
      {"content", "hello"}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

// ---- Directory-Based Rules ----

TEST_CASE("Allow directory for tool")
{
  mistral::agent::policy_engine engine;
  engine.allow_directory("write", "/home/user/project");

  mistral::agent::policy_context ctx{
    .tool_name = "write",
    .tool_arguments = mistral::agent::json{
      {"path", "/home/user/project/src/main.cpp"},
      {"content", "code"}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Allow directory does not allow outside directory")
{
  mistral::agent::policy_engine engine;
  engine.allow_directory("write", "/home/user/project");

  mistral::agent::policy_context ctx{
    .tool_name = "write",
    .tool_arguments = mistral::agent::json{
      {"path", "/tmp/file.txt"},
      {"content", "code"}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::DENY);
}

TEST_CASE("Allow directory with exclusions")
{
  mistral::agent::policy_engine engine;
  engine.allow_directory("write", "/home/user/project", {"build/", "dist/"});

  // Should allow in src/
  mistral::agent::policy_context ctx1{
    .tool_name = "write",
    .tool_arguments = mistral::agent::json{
      {"path", "/home/user/project/src/main.cpp"},
      {"content", "code"}
    },
    .working_directory = "/home/user"
  };
  CHECK(ctx1.tool_arguments.is_object());
  auto result1 = engine.evaluate(ctx1);
  CHECK(result1.decision == mistral::agent::policy_decision::ALLOW);

  // Should NOT allow in build/
  mistral::agent::policy_context ctx2{
    .tool_name = "write",
    .tool_arguments = mistral::agent::json{
      {"path", "/home/user/project/build/output.txt"},
      {"content", "code"}
    },
    .working_directory = "/home/user"
  };
  CHECK(ctx2.tool_arguments.is_object());
  auto result2 = engine.evaluate(ctx2);
  CHECK(result2.decision == mistral::agent::policy_decision::DENY);
}

TEST_CASE("Require approval for directory outside path")
{
  mistral::agent::policy_engine engine;
  engine.require_approval_directory("write", "!/home/user/project");

  // Outside project - should require approval
  mistral::agent::policy_context ctx1{
    .tool_name = "write",
    .tool_arguments = mistral::agent::json{
      {"path", "/tmp/file.txt"},
      {"content", "code"}
    },
    .working_directory = "/home/user"
  };
  CHECK(ctx1.tool_arguments.is_object());
  auto result1 = engine.evaluate(ctx1);
  CHECK(result1.decision == mistral::agent::policy_decision::REQUIRE_APPROVAL);

  // Inside project - should NOT require approval (with default DENY, it would be denied)
  mistral::agent::policy_context ctx2{
    .tool_name = "write",
    .tool_arguments = mistral::agent::json{
      {"path", "/home/user/project/src/main.cpp"},
      {"content", "code"}
    },
    .working_directory = "/home/user"
  };
  CHECK(ctx2.tool_arguments.is_object());
  auto result2 = engine.evaluate(ctx2);
  CHECK(result2.decision == mistral::agent::policy_decision::DENY);
}

// ---- Path Prefix Rules ----

TEST_CASE("Allow path prefix")
{
  mistral::agent::policy_engine engine;
  engine.allow_directory("read", "/home/user/safe");

  mistral::agent::policy_context ctx{
    .tool_name = "read",
    .tool_arguments = mistral::agent::json{
      {"path", "/home/user/safe/docs/README.md"}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Path prefix does not match partial path")
{
  mistral::agent::policy_engine engine;
  engine.allow_directory("read", "/home/user/safe");

  // /home/user/other should NOT match /home/user/safe
  // Note: /home/user/safe2 would match because it starts with /home/user/safe
  // So we use a completely different path
  mistral::agent::policy_context ctx{
    .tool_name = "read",
    .tool_arguments = mistral::agent::json{
      {"path", "/home/user/other/docs/README.md"}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::DENY);
}

// ---- DSL Rule Parsing ----

TEST_CASE("Parse allow tool DSL")
{
  mistral::agent::policy_engine engine;
  engine.add_rule("allow:tool=ls");

  mistral::agent::policy_context ctx{
    .tool_name = "ls",
    .tool_arguments = mistral::agent::json{
      {"path", "/tmp"}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Parse require tool DSL")
{
  mistral::agent::policy_engine engine;
  engine.add_rule("require:tool=write");

  mistral::agent::policy_context ctx{
    .tool_name = "write",
    .tool_arguments = mistral::agent::json{
      {"path", "/tmp/file.txt"},
      {"content", "hello"}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::REQUIRE_APPROVAL);
}

TEST_CASE("Parse allow directory DSL")
{
  mistral::agent::policy_engine engine;
  engine.add_rule("allow:write:dir=/home/user/project");

  mistral::agent::policy_context ctx{
    .tool_name = "write",
    .tool_arguments = mistral::agent::json{
      {"path", "/home/user/project/src/main.cpp"},
      {"content", "code"}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Parse allow directory with exclusions DSL")
{
  mistral::agent::policy_engine engine;
  engine.add_rule("allow:write:dir=/home/user/project,exclude=build/,dist/");

  // Should allow in src/
  mistral::agent::policy_context ctx1{
    .tool_name = "write",
    .tool_arguments = mistral::agent::json{
      {"path", "/home/user/project/src/main.cpp"},
      {"content", "code"}
    },
    .working_directory = "/home/user"
  };
  CHECK(ctx1.tool_arguments.is_object());
  auto result1 = engine.evaluate(ctx1);
  CHECK(result1.decision == mistral::agent::policy_decision::ALLOW);

  // Should NOT allow in build/
  mistral::agent::policy_context ctx2{
    .tool_name = "write",
    .tool_arguments = mistral::agent::json{
      {"path", "/home/user/project/build/output.txt"},
      {"content", "code"}
    },
    .working_directory = "/home/user"
  };
  CHECK(ctx2.tool_arguments.is_object());
  auto result2 = engine.evaluate(ctx2);
  CHECK(result2.decision == mistral::agent::policy_decision::DENY);
}

TEST_CASE("Parse require approval directory DSL with negation")
{
  mistral::agent::policy_engine engine;
  engine.add_rule("require:write:dir=!/home/user/project");

  // Outside project - should require approval
  mistral::agent::policy_context ctx{
    .tool_name = "write",
    .tool_arguments = mistral::agent::json{
      {"path", "/tmp/file.txt"},
      {"content", "code"}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::REQUIRE_APPROVAL);
}

// ---- JSON Configuration Loading ----

TEST_CASE("Load configuration from JSON - allow tool")
{
  mistral::agent::policy_engine engine;
  mistral::agent::json config = {
    {"default", "deny"},
    {
      "allow", mistral::agent::json{
        {
          {"type", "tool"}, {"tool", "ls"}
        }
      }
    }
  };

  CHECK(config.is_object());

  engine.load_configuration(config);

  mistral::agent::policy_context ctx{
    .tool_name = "ls",
    .tool_arguments = mistral::agent::json{
      {"path", "/tmp"}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Load configuration from JSON - allow directory")
{
  mistral::agent::policy_engine engine;
  mistral::agent::json config = {
    {"default", "deny"},
    {
      "allow", mistral::agent::json{
        {
          {"type", "directory"},
          {"tool", "write"},
          {"path", "/home/user/project"}
        }
      }
    }
  };

  CHECK(config.is_object());

  engine.load_configuration(config);

  mistral::agent::policy_context ctx{
    .tool_name = "write",
    .tool_arguments = mistral::agent::json{
      {"path", "/home/user/project/src/main.cpp"},
      {"content", "code"}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Load configuration from JSON - require approval")
{
  mistral::agent::policy_engine engine;
  mistral::agent::json config = {
    {"default", "allow"},
    {
      "require_approval", mistral::agent::json{
        {
          {"type", "tool"}, {"tool", "write"}
        }
      }
    }
  };

  CHECK(config.is_object());

  engine.load_configuration(config);

  mistral::agent::policy_context ctx{
    .tool_name = "write",
    .tool_arguments = mistral::agent::json{
      {"path", "/tmp/file.txt"},
      {"content", "code"}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::REQUIRE_APPROVAL);
}

TEST_CASE("Load configuration from JSON - rejection messages")
{
  mistral::agent::policy_engine engine;
  mistral::agent::json config = {
    {"default", "deny"},
    {
      "require_approval", mistral::agent::json{
        {
          {"type", "tool"}, {"tool", "write"}, {"id", "write_rule"}
        }
      }
    },
    {"rejection_messages", {
      {"write_rule", "Custom write rejection message"}
    }}
  };

  CHECK(config.is_object());

  engine.load_configuration(config);

  mistral::agent::policy_context ctx{
    .tool_name = "write",
    .tool_arguments = mistral::agent::json{
      {"path", "/tmp/file.txt"},
      {"content", "code"}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::REQUIRE_APPROVAL);
  CHECK(result.rejection_message.has_value());
  CHECK(*result.rejection_message == "Custom write rejection message");
}

// ---- Rule Ordering ----

TEST_CASE("Allow rule takes precedence over require approval")
{
  mistral::agent::policy_engine engine;
  engine.require_approval_tool("write");
  engine.allow_directory("write", "/home/user/project");

  mistral::agent::policy_context ctx{
    .tool_name = "write",
    .tool_arguments = mistral::agent::json{
      {"path", "/home/user/project/src/main.cpp"},
      {"content", "code"}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Specific rule takes precedence over general rule")
{
  mistral::agent::policy_engine engine;
  engine.allow_tool("write");  // General: allow all write
  engine.require_approval_directory("write", "!/home/user/project");  // Specific: require approval outside project

  // Outside project - should require approval (specific rule)
  mistral::agent::policy_context ctx1{
    .tool_name = "write",
    .tool_arguments = mistral::agent::json{
      {"path", "/tmp/file.txt"},
      {"content", "code"}
    },
    .working_directory = "/home/user"
  };
  CHECK(ctx1.tool_arguments.is_object());
  auto result1 = engine.evaluate(ctx1);
  CHECK(result1.decision == mistral::agent::policy_decision::REQUIRE_APPROVAL);

  // Inside project - should be allowed (general rule)
  mistral::agent::policy_context ctx2{
    .tool_name = "write",
    .tool_arguments = mistral::agent::json{
      {"path", "/home/user/project/src/main.cpp"},
      {"content", "code"}
    },
    .working_directory = "/home/user"
  };
  CHECK(ctx2.tool_arguments.is_object());
  auto result2 = engine.evaluate(ctx2);
  CHECK(result2.decision == mistral::agent::policy_decision::ALLOW);
}

// ---- Path Normalization ----

TEST_CASE("Path normalization resolves relative paths")
{
  std::string normalized = mistral::agent::policy_engine::normalize_path("./file.txt");
  // Should be an absolute path
  CHECK(!normalized.empty());
  CHECK(normalized[0] == '/');
  CHECK(mistral::agent::string_ends_with(normalized, "file.txt"));
}

TEST_CASE("Path normalization handles parent directory references")
{
  std::string original = "/home/user/../user/project/./src/../src/main.cpp";
  std::string normalized = mistral::agent::policy_engine::normalize_path(original);
  // Should resolve to /home/user/project/src/main.cpp or similar
  CHECK(normalized.find("/../") == std::string::npos);
  CHECK(normalized.find("/./") == std::string::npos);
}

// ---- Clear Rules ----

TEST_CASE("Clear rules removes all rules")
{
  mistral::agent::policy_engine engine;
  engine.allow_tool("ls");
  engine.allow_tool("read");

  CHECK(engine.rule_count() == 2);

  engine.clear_rules();
  CHECK(engine.rule_count() == 0);

  // After clearing, default policy should apply
  mistral::agent::policy_context ctx{
    .tool_name = "ls",
    .tool_arguments = mistral::agent::json{
      {"path", "/tmp"}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::DENY);
}

// ---- Edge Cases ----

TEST_CASE("Tool without path argument")
{
  mistral::agent::policy_engine engine;
  engine.allow_tool("ls");

  mistral::agent::policy_context ctx{
    .tool_name = "ls",
    .tool_arguments = mistral::agent::json::object(),  // Empty object, not json{}
    .working_directory = "/home/user"
  };

  // json::object() creates an empty object
  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Directory rule with tool that has no path argument")
{
  mistral::agent::policy_engine engine;
  engine.allow_directory("ls", "/home/user/project");

  mistral::agent::policy_context ctx{
    .tool_name = "ls",
    .tool_arguments = mistral::agent::json::object(),  // Empty object, not json{}
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  // Should not match because there's no path to check
  CHECK(result.decision == mistral::agent::policy_decision::DENY);
}

TEST_CASE("Wildcard tool name matches any tool")
{
  mistral::agent::policy_engine engine;
  engine.allow_directory("*", "/home/user/project");

  mistral::agent::policy_context ctx{
    .tool_name = "write",
    .tool_arguments = mistral::agent::json{
      {"path", "/home/user/project/src/main.cpp"},
      {"content", "code"}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

// ---- Executable Rules (Phase 2A) ----

TEST_CASE("Allow specific executable command")
{
  mistral::agent::policy_engine engine;
  engine.allow_executable("exec", "git");

  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "git"},
      {"args", {"status"}}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Allow executable does not allow different command")
{
  mistral::agent::policy_engine engine;
  engine.allow_executable("exec", "git");

  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "cmake"},
      {"args", {"--version"}}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::DENY);
}

TEST_CASE("Require approval for specific executable command")
{
  mistral::agent::policy_engine engine;
  engine.require_approval_executable("exec", "git");

  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "git"},
      {"args", {"push"}}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::REQUIRE_APPROVAL);
}

TEST_CASE("Executable rule with glob pattern")
{
  mistral::agent::policy_engine engine;
  engine.allow_executable("exec", "git*");

  // Should match git
  mistral::agent::policy_context ctx1{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "git"},
      {"args", {"status"}}
    },
    .working_directory = "/home/user"
  };
  CHECK(ctx1.tool_arguments.is_object());
  auto result1 = engine.evaluate(ctx1);
  CHECK(result1.decision == mistral::agent::policy_decision::ALLOW);

  // Should match git-foo
  mistral::agent::policy_context ctx2{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "git-foo"},
      {"args", {}}
    },
    .working_directory = "/home/user"
  };
  CHECK(ctx2.tool_arguments.is_object());
  auto result2 = engine.evaluate(ctx2);
  CHECK(result2.decision == mistral::agent::policy_decision::ALLOW);

  // Should NOT match cmake
  mistral::agent::policy_context ctx3{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "cmake"},
      {"args", {}}
    },
    .working_directory = "/home/user"
  };
  CHECK(ctx3.tool_arguments.is_object());
  auto result3 = engine.evaluate(ctx3);
  CHECK(result3.decision == mistral::agent::policy_decision::DENY);
}

TEST_CASE("Executable rule takes precedence over tool rule")
{
  mistral::agent::policy_engine engine;
  engine.allow_tool("exec");  // General: allow all exec
  engine.require_approval_executable("exec", "git");  // Specific: require approval for git

  // git should require approval (specific rule)
  mistral::agent::policy_context ctx1{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "git"},
      {"args", {"status"}}
    },
    .working_directory = "/home/user"
  };
  CHECK(ctx1.tool_arguments.is_object());
  auto result1 = engine.evaluate(ctx1);
  CHECK(result1.decision == mistral::agent::policy_decision::REQUIRE_APPROVAL);

  // cmake should be allowed (general rule)
  mistral::agent::policy_context ctx2{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "cmake"},
      {"args", {"--version"}}
    },
    .working_directory = "/home/user"
  };
  CHECK(ctx2.tool_arguments.is_object());
  auto result2 = engine.evaluate(ctx2);
  CHECK(result2.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Parse allow executable DSL")
{
  mistral::agent::policy_engine engine;
  engine.add_rule("allow:exec:command=git");

  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "git"},
      {"args", {"status"}}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Parse require executable DSL")
{
  mistral::agent::policy_engine engine;
  engine.add_rule("require:exec:command=git");

  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "git"},
      {"args", {"push"}}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::REQUIRE_APPROVAL);
}

TEST_CASE("Load executable rule from JSON - allow")
{
  mistral::agent::policy_engine engine;
  mistral::agent::json config = {
    {"default", "deny"},
    {
      "allow", mistral::agent::json{
        {
          {"type", "executable"}, {"tool", "exec"}, {"command", "git"}
        }
      }
    }
  };

  CHECK(config.is_object());

  engine.load_configuration(config);

  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "git"},
      {"args", {"status"}}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Load executable rule from JSON - require approval")
{
  mistral::agent::policy_engine engine;
  mistral::agent::json config = {
    {"default", "allow"},
    {
      "require_approval", mistral::agent::json{
        {
          {"type", "executable"}, {"tool", "exec"}, {"command", "git"}
        }
      }
    }
  };

  CHECK(config.is_object());

  engine.load_configuration(config);

  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "git"},
      {"args", {"push"}}
    },
    .working_directory = "/home/user"
  };

  CHECK(ctx.tool_arguments.is_object());

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::REQUIRE_APPROVAL);
}

TEST_SUITE_END();

// ============================================================================
// Phase 2B: Argument and CWD Constraints for Executable Rules
// ============================================================================

TEST_SUITE_BEGIN("Phase2B_ExecutableConstraints");

TEST_CASE("Allow executable with specific args - exact match")
{
  mistral::agent::policy_engine engine;
  engine.allow_executable("exec", "git", {"status", "log", "diff"});

  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "git"},
      {"args", {"status"}}
    },
    .working_directory = "/home/user"
  };

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Allow executable with specific args - multiple args match")
{
  mistral::agent::policy_engine engine;
  engine.allow_executable("exec", "git", {"status", "log"});

  // Should match because one of the args (status) is in the allowed list
  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "git"},
      {"args", {"status", "other"}}
    },
    .working_directory = "/home/user"
  };

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Allow executable with specific args - no match")
{
  mistral::agent::policy_engine engine;
  engine.allow_executable("exec", "git", {"status", "log"});

  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "git"},
      {"args", {"push", "force"}}
    },
    .working_directory = "/home/user"
  };

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::DENY);
}

TEST_CASE("Allow executable with cwd constraint - exact match")
{
  mistral::agent::policy_engine engine;
  engine.allow_executable("exec", "cmake", {}, "build");

  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "cmake"},
      {"args", {"--build", "."}}
    },
    .working_directory = "build"
  };

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Allow executable with cwd constraint - subdirectory match")
{
  mistral::agent::policy_engine engine;
  engine.allow_executable("exec", "cmake", {}, "build");

  // Should match because "build/debug" starts with "build"
  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "cmake"},
      {"args", {"--build", "."}}
    },
    .working_directory = "build/debug"
  };

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Allow executable with cwd constraint - no match")
{
  mistral::agent::policy_engine engine;
  engine.allow_executable("exec", "cmake", {}, "build");

  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "cmake"},
      {"args", {"--build", "."}}
    },
    .working_directory = "src"
  };

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::DENY);
}

TEST_CASE("Combined: command + args + cwd all match")
{
  mistral::agent::policy_engine engine;
  engine.allow_executable("exec", "cmake", {"--build"}, "build");

  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "cmake"},
      {"args", {"--build", "."}}
    },
    .working_directory = "build"
  };

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Combined: command matches but cwd does not")
{
  mistral::agent::policy_engine engine;
  engine.allow_executable("exec", "cmake", {}, "build");

  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "cmake"},
      {"args", {"--build", "."}}
    },
    .working_directory = "src"
  };

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::DENY);
}

TEST_CASE("Combined: command matches but args do not")
{
  mistral::agent::policy_engine engine;
  engine.allow_executable("exec", "git", {"status"});

  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "git"},
      {"args", {"push"}}
    },
    .working_directory = "/home/user"
  };

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::DENY);
}

TEST_CASE("Executable with args and cwd - both constraints must match")
{
  mistral::agent::policy_engine engine;
  engine.allow_executable("exec", "cmake", {"--build"}, "build");

  // cmake with --build in build/ - should match
  mistral::agent::policy_context ctx1{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "cmake"},
      {"args", {"--build", "."}}
    },
    .working_directory = "build"
  };
  CHECK(engine.evaluate(ctx1).decision == mistral::agent::policy_decision::ALLOW);

  // cmake with --build but wrong cwd - should not match
  mistral::agent::policy_context ctx2{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "cmake"},
      {"args", {"--build", "."}}
    },
    .working_directory = "src"
  };
  CHECK(engine.evaluate(ctx2).decision == mistral::agent::policy_decision::DENY);

  // cmake in build/ but wrong args - should not match
  mistral::agent::policy_context ctx3{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "cmake"},
      {"args", {"--version"}}
    },
    .working_directory = "build"
  };
  CHECK(engine.evaluate(ctx3).decision == mistral::agent::policy_decision::DENY);
}

TEST_CASE("Parse DSL with args constraint")
{
  mistral::agent::policy_engine engine;
  engine.add_rule("allow:exec:command=git,args=status,log");

  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "git"},
      {"args", {"status"}}
    },
    .working_directory = "/home/user"
  };

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Parse DSL with cwd constraint")
{
  mistral::agent::policy_engine engine;
  engine.add_rule("allow:exec:command=cmake,cwd=build");

  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "cmake"},
      {"args", {"--build", "."}}
    },
    .working_directory = "build"
  };

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Parse DSL with both args and cwd constraints")
{
  mistral::agent::policy_engine engine;
  engine.add_rule("allow:exec:command=cmake,args=--build,--version,cwd=build");

  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "cmake"},
      {"args", {"--build", "."}}
    },
    .working_directory = "build"
  };

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Load from JSON with args and cwd")
{
  mistral::agent::policy_engine engine;
  mistral::agent::json config = {
    {"default", "deny"},
    {
      "allow", mistral::agent::json{
        {
          {"type", "executable"}, {"tool", "exec"}, {"command", "cmake"}, {"args", mistral::agent::json::array({"--build"})}, {"cwd", "build"}
        }
      }
    }
  };

  engine.load_configuration(config);

  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "cmake"},
      {"args", {"--build", "."}}
    },
    .working_directory = "build"
  };

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Load from JSON with args and cwd - no match on args")
{
  mistral::agent::policy_engine engine;
  mistral::agent::json config = {
    {"default", "deny"},
    {
      "allow", mistral::agent::json{
        {
          {"type", "executable"}, {"tool", "exec"}, {"command", "cmake"}, {"args", mistral::agent::json::array({"--build"})}, {"cwd", "build"}
        }
      }
    }
  };

  engine.load_configuration(config);

  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "cmake"},
      {"args", {"--version"}}  // Not in allowed args
    },
    .working_directory = "build"
  };

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::DENY);
}

TEST_CASE("Load from JSON with args and cwd - no match on cwd")
{
  mistral::agent::policy_engine engine;
  mistral::agent::json config = {
    {"default", "deny"},
    {
      "allow", mistral::agent::json{
        {
          {"type", "executable"}, {"tool", "exec"}, {"command", "cmake"}, {"args", mistral::agent::json::array({"--build"})}, {"cwd", "build"}
        }
      }
    }
  };

  engine.load_configuration(config);

  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "cmake"},
      {"args", {"--build", "."}}
    },
    .working_directory = "src"  // Not in allowed cwd
  };

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::DENY);
}

TEST_CASE("Priority: combined rule overrides command-only rule")
{
  mistral::agent::policy_engine engine;
  // General: allow all cmake commands
  engine.allow_executable("exec", "cmake");
  // Specific: require approval for cmake in build/ with --install
  engine.require_approval_executable("exec", "cmake", {"--install"}, "build");

  // cmake --install in build/ should require approval (specific rule)
  mistral::agent::policy_context ctx1{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "cmake"},
      {"args", {"--install"}}
    },
    .working_directory = "build"
  };
  CHECK(engine.evaluate(ctx1).decision == mistral::agent::policy_decision::REQUIRE_APPROVAL);

  // cmake --build in build/ should be allowed (general rule)
  mistral::agent::policy_context ctx2{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "cmake"},
      {"args", {"--build", "."}}
    },
    .working_directory = "build"
  };
  CHECK(engine.evaluate(ctx2).decision == mistral::agent::policy_decision::ALLOW);

  // cmake --install in src/ should be allowed (general rule, cwd doesn't match specific)
  mistral::agent::policy_context ctx3{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "cmake"},
      {"args", {"--install"}}
    },
    .working_directory = "src"
  };
  CHECK(engine.evaluate(ctx3).decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Glob patterns in args")
{
  mistral::agent::policy_engine engine;
  engine.allow_executable("exec", "git", {"status*", "log*"});

  // Should match git status
  mistral::agent::policy_context ctx1{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "git"},
      {"args", {"status"}}
    },
    .working_directory = "/home/user"
  };
  CHECK(engine.evaluate(ctx1).decision == mistral::agent::policy_decision::ALLOW);

  // Should match git status--short
  mistral::agent::policy_context ctx2{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "git"},
      {"args", {"status--short"}}
    },
    .working_directory = "/home/user"
  };
  CHECK(engine.evaluate(ctx2).decision == mistral::agent::policy_decision::ALLOW);

  // Should NOT match git push
  mistral::agent::policy_context ctx3{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "git"},
      {"args", {"push"}}
    },
    .working_directory = "/home/user"
  };
  CHECK(engine.evaluate(ctx3).decision == mistral::agent::policy_decision::DENY);
}

TEST_CASE("Empty args in context - rule has args constraint")
{
  mistral::agent::policy_engine engine;
  engine.allow_executable("exec", "git", {"status"});

  // Empty args array
  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "git"},
      {"args", {}}
    },
    .working_directory = "/home/user"
  };

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::DENY);
}

TEST_CASE("No args field in context - rule has args constraint")
{
  mistral::agent::policy_engine engine;
  engine.allow_executable("exec", "git", {"status"});

  // No args field at all
  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "git"}
    },
    .working_directory = "/home/user"
  };

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::DENY);
}

TEST_CASE("Empty args in rule - any args allowed")
{
  mistral::agent::policy_engine engine;
  engine.allow_executable("exec", "cmake", {}, "build");

  // Any args should be fine as long as cwd matches
  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "cmake"},
      {"args", {"any", "args", "here"}}
    },
    .working_directory = "build"
  };

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("No cwd in rule - any cwd allowed")
{
  mistral::agent::policy_engine engine;
  engine.allow_executable("exec", "git", {"status"});

  // Any cwd should be fine as long as args match
  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "git"},
      {"args", {"status"}}
    },
    .working_directory = "/any/directory"
  };

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Exec rule with cwd matches when working_directory is empty")
{
  // Simulates the real-world scenario: evaluate_tool_policy sets
  // working_directory to "" and the AI doesn't pass cwd in exec args.
  // The rule has cwd="." which should match the current directory.
  mistral::agent::policy_engine engine;
  engine.allow_executable("exec", "make", {}, ".");

  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "make"},
      {"args", mistral::agent::json::array()}
    },
    .working_directory = ""
  };

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Exec rule with cwd matches when cwd passed in tool arguments")
{
  // When the AI passes cwd in the exec tool arguments, it should be
  // used for policy matching against the rule's cwd constraint.
  mistral::agent::policy_engine engine;
  engine.allow_executable("exec", "make", {}, ".");

  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "make"},
      {"args", mistral::agent::json::array()},
      {"cwd", "."}
    },
    .working_directory = ""
  };

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::ALLOW);
}

TEST_CASE("Exec rule with cwd does not match different cwd in tool arguments")
{
  mistral::agent::policy_engine engine;
  engine.allow_executable("exec", "make", {}, "build");

  mistral::agent::policy_context ctx{
    .tool_name = "exec",
    .tool_arguments = mistral::agent::json{
      {"command", "make"},
      {"args", mistral::agent::json::array()},
      {"cwd", "src"}
    },
    .working_directory = ""
  };

  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::DENY);
}

TEST_CASE("MCP server tool rule matches only connected server tools")
{
  // Deterministic regardless of other tests: no servers configured.
  mistral::mcp::mcp_manager::instance().configure({});

  mistral::agent::policy_engine engine;
  engine.load_configuration(mistral::agent::json{
    {"default", "deny"},
    {"allow", mistral::agent::json::array({
      mistral::agent::json{{"type", "tool"}, {"tool", "mcp:storybook"}}
    })}
  });

  mistral::agent::policy_context ctx{
    .tool_name = "stories-preview",
    .tool_arguments = mistral::agent::json::object(),
    .working_directory = "/home/user"
  };

  // Server not connected: the mcp: rule matches nothing, so the
  // default policy applies.
  auto result = engine.evaluate(ctx);
  CHECK(result.decision == mistral::agent::policy_decision::DENY);

  // A non-MCP tool is unaffected by the rule.
  mistral::agent::policy_context ctx2{
    .tool_name = "ls",
    .tool_arguments = mistral::agent::json::object(),
    .working_directory = "/home/user"
  };
  auto result2 = engine.evaluate(ctx2);
  CHECK(result2.decision == mistral::agent::policy_decision::DENY);
}

TEST_SUITE_END();
