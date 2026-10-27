#include "doctest/doctest.h"
#include "template/template_engine.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace
{
  // Helper to create a temp directory with files for include tests.
  struct temp_dir
  {
    fs::path path;

    temp_dir()
    {
      path = fs::temp_directory_path() / "mistral_tmpl_test";
      fs::create_directories(path);
    }

    ~temp_dir()
    {
      fs::remove_all(path);
    }

    void write(const std::string& name, const std::string& content)
    {
      auto p = path / name;
      fs::create_directories(p.parent_path());
      std::ofstream f(p);
      f << content;
    }
  };
} // namespace

TEST_CASE("template_engine: variable substitution")
{
  mistral::tmpl::template_engine engine;
  engine.set_variable("name", "World");
  engine.set_variable("model", "mistral-medium");

  CHECK(engine.process("Hello ${name}!") == "Hello World!");
  CHECK(engine.process("Using ${model} today") == "Using mistral-medium today");
}

TEST_CASE("template_engine: unknown variable returns empty")
{
  mistral::tmpl::template_engine engine;
  CHECK(engine.process("${unknown}") == "");
}

TEST_CASE("template_engine: env variable fallback")
{
  mistral::tmpl::template_engine engine;
  // HOME should always be set.
  auto result = engine.process("${HOME}");
  CHECK(!result.empty());
}

TEST_CASE("template_engine: shell command expansion")
{
  mistral::tmpl::template_engine engine;
  CHECK(engine.process("${{echo hello}}") == "hello");
}

TEST_CASE("template_engine: command expansion disabled")
{
  mistral::tmpl::template_engine engine;
  engine.enable_command_expansion(false);
  auto result = engine.process("${{echo hello}}");
  // Should preserve the directive unexpanded.
  CHECK(result.find("echo hello") != std::string::npos);
}

TEST_CASE("template_engine: file include")
{
  temp_dir tmp;
  tmp.write("greeting.md", "Hello from partial!");

  mistral::tmpl::template_engine engine;
  engine.set_include_base(tmp.path);

  CHECK(engine.process("Before {{> greeting}} After") == "Before Hello from partial! After");
}

TEST_CASE("template_engine: include without .md extension")
{
  temp_dir tmp;
  tmp.write("snippet.md", "I am a snippet");

  mistral::tmpl::template_engine engine;
  engine.set_include_base(tmp.path);

  CHECK(engine.process("{{> snippet}}") == "I am a snippet");
}

TEST_CASE("template_engine: nested includes")
{
  temp_dir tmp;
  tmp.write("inner.md", "INNER");
  tmp.write("outer.md", "before-{{> inner}}-after");

  mistral::tmpl::template_engine engine;
  engine.set_include_base(tmp.path);

  CHECK(engine.process("{{> outer}}") == "before-INNER-after");
}

TEST_CASE("template_engine: circular include detection")
{
  temp_dir tmp;
  tmp.write("a.md", "{{> b}}");
  tmp.write("b.md", "{{> a}}");

  mistral::tmpl::template_engine engine;
  engine.set_include_base(tmp.path);

  CHECK_THROWS_AS(engine.process("{{> a}}"), std::runtime_error);
}

TEST_CASE("template_engine: include with variables")
{
  temp_dir tmp;
  tmp.write("greet.md", "Hello ${name}!");

  mistral::tmpl::template_engine engine;
  engine.set_include_base(tmp.path);
  engine.set_variable("name", "Alice");

  CHECK(engine.process("{{> greet}}") == "Hello Alice!");
}

TEST_CASE("template_engine: mixed directives")
{
  temp_dir tmp;
  tmp.write("sig.md", "-- ${author}");

  mistral::tmpl::template_engine engine;
  engine.set_include_base(tmp.path);
  engine.set_variable("author", "Bot");

  auto result = engine.process("Date: ${{date +%Y}}\n{{> sig}}");
  CHECK(result.find("-- Bot") != std::string::npos);
  // Date should be a 4-digit year.
  CHECK(result.find("202") != std::string::npos);
}

TEST_CASE("template_engine: process_file")
{
  temp_dir tmp;
  tmp.write("prompt.md", "You are ${role}. Model: ${model}.");

  mistral::tmpl::template_engine engine;
  engine.set_variable("role", "an assistant");
  engine.set_variable("model", "test");

  auto result = engine.process_file(tmp.path / "prompt.md");
  CHECK(result == "You are an assistant. Model: test.");
}

TEST_CASE("template_engine: passthrough when no directives")
{
  mistral::tmpl::template_engine engine;
  std::string plain = "Just a plain string with {braces} and $dollars.";
  CHECK(engine.process(plain) == plain);
}

TEST_CASE("template_engine: conditional include if= (truthy)")
{
  temp_dir tmp;
  tmp.write("extra.md", "EXTRA CONTENT");

  mistral::tmpl::template_engine engine;
  engine.set_include_base(tmp.path);
  engine.set_variable("feature", "true");

  CHECK(engine.process("A {{> extra if=feature}} B") == "A EXTRA CONTENT B");
}

TEST_CASE("template_engine: conditional include if= (falsy)")
{
  temp_dir tmp;
  tmp.write("extra.md", "EXTRA CONTENT");

  mistral::tmpl::template_engine engine;
  engine.set_include_base(tmp.path);
  engine.set_variable("feature", "false");

  CHECK(engine.process("A {{> extra if=feature}} B") == "A  B");
}

TEST_CASE("template_engine: conditional include if= (unset)")
{
  temp_dir tmp;
  tmp.write("extra.md", "EXTRA CONTENT");

  mistral::tmpl::template_engine engine;
  engine.set_include_base(tmp.path);
  // Variable not set — should be falsy.

  CHECK(engine.process("A {{> extra if=feature}} B") == "A  B");
}

TEST_CASE("template_engine: conditional include unless= (truthy)")
{
  temp_dir tmp;
  tmp.write("extra.md", "EXTRA CONTENT");

  mistral::tmpl::template_engine engine;
  engine.set_include_base(tmp.path);
  engine.set_variable("feature", "true");

  CHECK(engine.process("A {{> extra unless=feature}} B") == "A  B");
}

TEST_CASE("template_engine: conditional include unless= (falsy)")
{
  temp_dir tmp;
  tmp.write("extra.md", "EXTRA CONTENT");

  mistral::tmpl::template_engine engine;
  engine.set_include_base(tmp.path);
  engine.set_variable("feature", "false");

  CHECK(engine.process("A {{> extra unless=feature}} B") == "A EXTRA CONTENT B");
}

TEST_CASE("template_engine: conditional include with missing file (optional)")
{
  temp_dir tmp;
  // File does not exist.

  mistral::tmpl::template_engine engine;
  engine.set_include_base(tmp.path);
  engine.set_variable("feature", "true");

  // Should silently skip, not throw.
  CHECK(engine.process("A {{> missing if=feature}} B") == "A  B");
}

TEST_CASE("template_engine: truthy values")
{
  temp_dir tmp;
  tmp.write("extra.md", "X");

  mistral::tmpl::template_engine engine;
  engine.set_include_base(tmp.path);

  // All these should be truthy.
  for (const char* val : {"true", "TRUE", "True", "1", "yes", "YES", "on", "ON"})
  {
    engine.set_variable("v", val);
    CHECK(engine.process("{{> extra if=v}}") == "X");
  }
}

TEST_CASE("template_engine: falsy values")
{
  temp_dir tmp;
  tmp.write("extra.md", "X");

  mistral::tmpl::template_engine engine;
  engine.set_include_base(tmp.path);

  // All these should be falsy.
  for (const char* val : {"false", "FALSE", "0", "no", "NO", "off", "OFF", ""})
  {
    engine.set_variable("v", val);
    CHECK(engine.process("{{> extra if=v}}") == "");
  }
}
