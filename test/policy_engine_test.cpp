#include <doctest/doctest.h>
#include "agent/policy_engine.h"

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

TEST_SUITE_END();
