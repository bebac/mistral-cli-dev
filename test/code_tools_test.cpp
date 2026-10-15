#include <doctest/doctest.h>
#include "tools/code_tools.h"

#include <dripcore/scheduler.h>
#include <dripcore/task.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace mistral::tools
{
  namespace
  {
    // Helper tasks to run the async code_tools calls from a test.
    class outline_task : public dripcore::task
    {
    public:
      outline_task(code_tools& tools, std::string path, json& out)
        : tools_(tools), path_(std::move(path)), out_(out) {}

      dripcore::awaitable<> body() override
      {
        out_ = co_await tools_.outline(path_);
        co_return;
      }

    private:
      code_tools& tools_;
      std::string path_;
      json& out_;
    };

    class read_symbol_task : public dripcore::task
    {
    public:
      read_symbol_task(code_tools& tools, std::string path, std::string name, json& out)
        : tools_(tools), path_(std::move(path)), name_(std::move(name)), out_(out) {}

      dripcore::awaitable<> body() override
      {
        out_ = co_await tools_.read_symbol(path_, name_);
        co_return;
      }

    private:
      code_tools& tools_;
      std::string path_;
      std::string name_;
      json& out_;
    };

    json run_outline(code_tools& tools, const std::string& path)
    {
      json result;
      dripcore::scheduler scheduler;
      scheduler.spawn<outline_task>(tools, path, result);
      scheduler.run();
      return result;
    }

    json run_read_symbol(code_tools& tools, const std::string& path, const std::string& name)
    {
      json result;
      dripcore::scheduler scheduler;
      scheduler.spawn<read_symbol_task>(tools, path, name, result);
      scheduler.run();
      return result;
    }

    const json* find_symbol(const json& result, const std::string& name)
    {
      if (!result.contains("symbols")) {
        return nullptr;
      }
      for (const auto& symbol : result["symbols"]) {
        if (symbol["name"] == name) {
          return &symbol;
        }
      }
      return nullptr;
    }

    // Writes a small C++ sample to a temp file and removes it on scope exit.
    struct sample_file
    {
      fs::path path;

      sample_file()
        : path(fs::temp_directory_path() / "mistral_code_tools_test.cpp")
      {
        std::ofstream out(path);
        out <<
          "class tool_registry {\n"                     // line 1
          "public:\n"                                   // line 2
          "  void register_tool(const char* name);\n"   // line 3
          "};\n"                                        // line 4
          "typedef int handle;\n"                       // line 5
          "enum class color { red };\n"                 // line 6
          "int main() { return 0; }\n";                 // line 7
      }

      ~sample_file()
      {
        std::error_code ec;
        fs::remove(path, ec);
      }
    };

    // Multi-file sample project for index tests.
    struct sample_project
    {
      fs::path root;

      sample_project()
        : root(fs::temp_directory_path() / "mistral_code_index_test")
      {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / "sub");
        fs::create_directories(root / "build");

        {
          std::ofstream out(root / "a.cpp");
          out << "int alpha() { return 1; }\n"
              << "int caller() { return alpha(); }\n";
        }
        {
          std::ofstream out(root / "sub" / "b.cpp");
          out << "class beta {\npublic:\n  int gamma();\n};\n";
        }
        {
          std::ofstream out(root / "build" / "c.cpp");
          out << "int excluded_symbol() { return 0; }\n";
        }
      }

      ~sample_project()
      {
        std::error_code ec;
        fs::remove_all(root, ec);
      }
    };

    // Sample with same-named methods in different classes, a namespace,
    // an out-of-line definition and a usage site, for qualified-name
    // lookup tests.
    struct qualified_sample
    {
      fs::path root;

      qualified_sample()
        : root(fs::temp_directory_path() / "mistral_code_qualified_test")
      {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root);
        {
          std::ofstream out(root / "q.cpp");
          out <<
            "namespace demo {\n"                    // line 1
            "class widget {\n"                      // line 2
            "public:\n"                             // line 3
            "  void draw();\n"                      // line 4
            "  int area() { return 4; }\n"           // line 5
            "};\n"                                  // line 6
            "class gadget {\n"                       // line 7
            "public:\n"                             // line 8
            "  void draw() { }\n"                   // line 9
            "};\n"                                  // line 10
            "void widget::draw() { }\n"             // line 11
            "}\n"                                   // line 12
            "int use() {\n"                         // line 13
            "  demo::widget w;\n"                   // line 14
            "  w.draw();\n"                         // line 15
            "  return 0;\n"                          // line 16
            "}\n";                                   // line 17
        }
      }

      ~qualified_sample()
      {
        std::error_code ec;
        fs::remove_all(root, ec);
      }
    };

    class find_symbol_task : public dripcore::task
    {
    public:
      find_symbol_task(code_tools& tools, std::string name, std::string path, json& out)
        : tools_(tools), name_(std::move(name)), path_(std::move(path)), out_(out) {}

      dripcore::awaitable<> body() override
      {
        out_ = co_await tools_.find_symbol(name_, path_);
        co_return;
      }

    private:
      code_tools& tools_;
      std::string name_;
      std::string path_;
      json& out_;
    };

    class find_references_task : public dripcore::task
    {
    public:
      find_references_task(code_tools& tools, std::string name, std::string path, json& out)
        : tools_(tools), name_(std::move(name)), path_(std::move(path)), out_(out) {}

      dripcore::awaitable<> body() override
      {
        out_ = co_await tools_.find_references(name_, path_);
        co_return;
      }

    private:
      code_tools& tools_;
      std::string name_;
      std::string path_;
      json& out_;
    };

    json run_find_symbol(code_tools& tools, const std::string& name, const std::string& path)
    {
      json result;
      dripcore::scheduler scheduler;
      scheduler.spawn<find_symbol_task>(tools, name, path, result);
      scheduler.run();
      return result;
    }

    json run_find_references(code_tools& tools, const std::string& name, const std::string& path)
    {
      json result;
      dripcore::scheduler scheduler;
      scheduler.spawn<find_references_task>(tools, name, path, result);
      scheduler.run();
      return result;
    }

    class ast_search_task : public dripcore::task
    {
    public:
      ast_search_task(code_tools& tools, std::string query, std::string path, json& out)
        : tools_(tools), query_(std::move(query)), path_(std::move(path)), out_(out) {}

      dripcore::awaitable<> body() override
      {
        out_ = co_await tools_.ast_search(query_, path_);
        co_return;
      }

    private:
      code_tools& tools_;
      std::string query_;
      std::string path_;
      json& out_;
    };

    json run_ast_search(code_tools& tools, const std::string& query, const std::string& path)
    {
      json result;
      dripcore::scheduler scheduler;
      scheduler.spawn<ast_search_task>(tools, query, path, result);
      scheduler.run();
      return result;
    }
  }
}

TEST_CASE("code_tools: outline lists symbols with kind and line ranges")
{
  mistral::tools::code_tools tools;
  mistral::tools::sample_file sample;

  auto result = mistral::tools::run_outline(tools, sample.path.string());

  REQUIRE(result.contains("symbols"));
  CHECK(result["count"].get<int>() == 5);

  const auto* symbol = mistral::tools::find_symbol(result, "tool_registry");
  REQUIRE(symbol != nullptr);
  CHECK((*symbol)["kind"] == "class");
  CHECK((*symbol)["line_start"].get<int>() == 1);
  CHECK((*symbol)["line_end"].get<int>() == 4);

  symbol = mistral::tools::find_symbol(result, "register_tool");
  REQUIRE(symbol != nullptr);
  CHECK((*symbol)["kind"] == "method");
  CHECK((*symbol)["scope"] == "tool_registry");
  CHECK((*symbol)["line_start"].get<int>() == 3);

  symbol = mistral::tools::find_symbol(result, "main");
  REQUIRE(symbol != nullptr);
  CHECK((*symbol)["kind"] == "function");

  symbol = mistral::tools::find_symbol(result, "handle");
  REQUIRE(symbol != nullptr);
  CHECK((*symbol)["kind"] == "type");

  symbol = mistral::tools::find_symbol(result, "color");
  REQUIRE(symbol != nullptr);
  CHECK((*symbol)["kind"] == "type");
}

TEST_CASE("code_tools: read_symbol extracts exact definition source")
{
  mistral::tools::code_tools tools;
  mistral::tools::sample_file sample;

  auto result = mistral::tools::run_read_symbol(
    tools, sample.path.string(), "main");
  REQUIRE(result.contains("definitions"));
  CHECK(result["count"].get<int>() == 1);
  CHECK(result["definitions"][0]["kind"] == "function");
  CHECK(result["definitions"][0]["content"] == "int main() { return 0; }");

  result = mistral::tools::run_read_symbol(
    tools, sample.path.string(), "register_tool");
  REQUIRE(result.contains("definitions"));
  CHECK(result["definitions"][0]["content"] == "void register_tool(const char* name);");

  result = mistral::tools::run_read_symbol(
    tools, sample.path.string(), "tool_registry");
  REQUIRE(result.contains("definitions"));
  // The class_specifier extent excludes the trailing semicolon.
  CHECK(result["definitions"][0]["content"] ==
    "class tool_registry {\n"
    "public:\n"
    "  void register_tool(const char* name);\n"
    "}");
  CHECK(result["definitions"][0]["line_start"].get<int>() == 1);
  CHECK(result["definitions"][0]["line_end"].get<int>() == 4);
}

TEST_CASE("code_tools: error handling")
{
  mistral::tools::code_tools tools;

  auto result = mistral::tools::run_outline(tools, "does_not_exist.cpp");
  CHECK(result.contains("error"));

  mistral::tools::sample_file sample;
  result = mistral::tools::run_read_symbol(
    tools, sample.path.string(), "no_such_symbol");
  CHECK(result.contains("error"));

  const fs::path not_code = fs::temp_directory_path() / "mistral_code_tools_test.txt";
  {
    std::ofstream out(not_code);
    out << "class foo {};";
  }
  result = mistral::tools::run_outline(tools, not_code.string());
  CHECK(result.contains("error"));
  fs::remove(not_code);
}

TEST_CASE("code_tools: find_symbol locates definitions across files")
{
  mistral::tools::code_tools tools;
  mistral::tools::sample_project project;

  auto result = mistral::tools::run_find_symbol(tools, "alpha", project.root.string());

  REQUIRE(result.contains("matches"));
  CHECK(result["match"] == "exact");
  REQUIRE(result["count"].get<int>() == 1);
  CHECK(result["matches"][0]["name"] == "alpha");
  CHECK(result["matches"][0]["kind"] == "function");
  CHECK(result["matches"][0]["line_start"].get<int>() == 1);
  const auto expected_a =
    fs::relative(project.root / "a.cpp", fs::current_path()).string();
  CHECK(result["matches"][0]["file"].get<std::string>() == expected_a);

  // Substring fallback when no exact match exists.
  result = mistral::tools::run_find_symbol(tools, "alp", project.root.string());
  CHECK(result["match"] == "substring");
  CHECK(result["count"].get<int>() == 1);

  // Excluded directories are not indexed.
  result = mistral::tools::run_find_symbol(tools, "excluded_symbol", project.root.string());
  CHECK(result["count"].get<int>() == 0);

  // Changed files are re-indexed (the stamp includes size).
  {
    std::ofstream out(project.root / "a.cpp", std::ios::app);
    out << "int delta() { return 3; }\n";
  }
  result = mistral::tools::run_find_symbol(tools, "delta", project.root.string());
  REQUIRE(result["count"].get<int>() == 1);
  CHECK(result["matches"][0]["name"] == "delta");
}

TEST_CASE("code_tools: find_symbol handles paths with trailing slashes")
{
  mistral::tools::code_tools tools;
  mistral::tools::sample_project project;

  // Baseline: absolute path without trailing slash.
  auto result = mistral::tools::run_find_symbol(tools, "alpha", project.root.string());
  REQUIRE(result["count"].get<int>() == 1);
  CHECK(result["matches"][0]["name"] == "alpha");

  // Absolute path with trailing slash should return the same results.
  const std::string root_with_slash = project.root.string() + "/";
  result = mistral::tools::run_find_symbol(tools, "alpha", root_with_slash);
  REQUIRE(result["count"].get<int>() == 1);
  CHECK(result["matches"][0]["name"] == "alpha");

  // Subdirectory with trailing slash should also work.
  const std::string sub_with_slash = (project.root / "sub").string() + "/";
  result = mistral::tools::run_find_symbol(tools, "gamma", sub_with_slash);
  REQUIRE(result["count"].get<int>() == 1);
  CHECK(result["matches"][0]["name"] == "gamma");

  // Subdirectory without trailing slash should work too.
  const std::string sub_no_slash = (project.root / "sub").string();
  result = mistral::tools::run_find_symbol(tools, "gamma", sub_no_slash);
  REQUIRE(result["count"].get<int>() == 1);
  CHECK(result["matches"][0]["name"] == "gamma");
}

TEST_CASE("code_tools: find_references finds usage sites, not definitions")
{
  mistral::tools::code_tools tools;
  mistral::tools::sample_project project;

  auto result = mistral::tools::run_find_references(tools, "alpha", project.root.string());

  REQUIRE(result.contains("matches"));
  REQUIRE(result["count"].get<int>() == 1);
  CHECK(result["matches"][0]["line"].get<int>() == 2);
  CHECK(result["matches"][0]["text"].get<std::string>() ==
    "int caller() { return alpha(); }");
  const auto expected_a =
    fs::relative(project.root / "a.cpp", fs::current_path()).string();
  CHECK(result["matches"][0]["file"].get<std::string>() == expected_a);
}

TEST_CASE("code_tools: qualified names select a specific method")
{
  mistral::tools::code_tools tools;
  mistral::tools::qualified_sample sample;
  const std::string file = (sample.root / "q.cpp").string();

  // Bare name: every same-named definition (widget declaration, gadget
  // definition, out-of-line widget definition).
  auto result = mistral::tools::run_read_symbol(tools, file, "draw");
  REQUIRE(result.contains("definitions"));
  CHECK(result["count"].get<int>() == 3);

  // Qualified: only widget's draw (declaration + out-of-line definition).
  result = mistral::tools::run_read_symbol(tools, file, "widget::draw");
  REQUIRE(result.contains("definitions"));
  CHECK(result["count"].get<int>() == 2);
  for (const auto& definition : result["definitions"]) {
    CHECK(definition["scope"] == "demo::widget");
    CHECK(definition["kind"] == "method");
  }

  // Namespace-qualified matches by scope suffix.
  result = mistral::tools::run_read_symbol(tools, file, "demo::widget::draw");
  REQUIRE(result.contains("definitions"));
  CHECK(result["count"].get<int>() == 2);

  // A different class narrows to a single definition.
  result = mistral::tools::run_read_symbol(tools, file, "gadget::draw");
  REQUIRE(result.contains("definitions"));
  CHECK(result["count"].get<int>() == 1);
  CHECK(result["definitions"][0]["scope"] == "demo::gadget");

  // Unknown scope: no match.
  result = mistral::tools::run_read_symbol(tools, file, "nosuch::draw");
  CHECK(result.contains("error"));

  // In-class member functions report method kind and their scope.
  result = mistral::tools::run_read_symbol(tools, file, "area");
  REQUIRE(result.contains("definitions"));
  CHECK(result["count"].get<int>() == 1);
  CHECK(result["definitions"][0]["kind"] == "method");
  CHECK(result["definitions"][0]["scope"] == "demo::widget");
}

TEST_CASE("code_tools: find_symbol honors qualified names")
{
  mistral::tools::code_tools tools;
  mistral::tools::qualified_sample sample;

  auto result = mistral::tools::run_find_symbol(
    tools, "widget::draw", sample.root.string());
  REQUIRE(result.contains("matches"));
  CHECK(result["match"] == "exact");
  REQUIRE(result["count"].get<int>() == 2);
  for (const auto& match : result["matches"]) {
    CHECK(match["scope"] == "demo::widget");
  }

  result = mistral::tools::run_find_symbol(
    tools, "demo::widget::draw", sample.root.string());
  REQUIRE(result["count"].get<int>() == 2);

  result = mistral::tools::run_find_symbol(
    tools, "gadget::draw", sample.root.string());
  REQUIRE(result["count"].get<int>() == 1);

  // Unknown scope falls back to substring matching on the bare name.
  result = mistral::tools::run_find_symbol(
    tools, "nosuch::draw", sample.root.string());
  CHECK(result["match"] == "substring");
  CHECK(result["count"].get<int>() == 3);
}

TEST_CASE("code_tools: find_references searches qualified names by bare name")
{
  mistral::tools::code_tools tools;
  mistral::tools::qualified_sample sample;

  auto result = mistral::tools::run_find_references(
    tools, "widget::draw", sample.root.string());

  REQUIRE(result.contains("matches"));
  REQUIRE(result["count"].get<int>() == 1);
  CHECK(result["matches"][0]["line"].get<int>() == 15);
  CHECK(result["matches"][0]["text"].get<std::string>() == "  w.draw();");
}

TEST_CASE("code_tools: results are capped at max_results")
{
  mistral::tools::code_index_settings settings;
  settings.max_results = 1;
  mistral::tools::code_tools tools(settings);
  mistral::tools::sample_project project;

  auto result = mistral::tools::run_find_symbol(tools, "a", project.root.string());

  REQUIRE(result.contains("matches"));
  CHECK(result["match"] == "substring");
  CHECK(result["count"].get<int>() == 1);
  CHECK(result["truncated"] == true);
}

TEST_CASE("code_tools: find_references results are capped at max_results")
{
  mistral::tools::code_index_settings settings;
  settings.max_results = 2;
  mistral::tools::code_tools tools(settings);
  mistral::tools::sample_project project;

  // Add call sites of alpha (4 usages total in a.cpp; the definition
  // itself is excluded).
  {
    std::ofstream out(project.root / "a.cpp", std::ios::app);
    out << "int more() { alpha(); alpha(); alpha(); }\n";
  }

  auto result = mistral::tools::run_find_references(tools, "alpha", project.root.string());

  REQUIRE(result.contains("matches"));
  CHECK(result["count"].get<int>() == 2);
  CHECK(result["truncated"] == true);
}

TEST_CASE("code_tools: ast_search directory results are capped at max_results")
{
  mistral::tools::code_index_settings settings;
  settings.max_results = 1;
  mistral::tools::code_tools tools(settings);
  mistral::tools::sample_project project;

  // a.cpp defines alpha and caller (two function_definitions).
  auto result = mistral::tools::run_ast_search(
    tools, "(function_definition) @fn", project.root.string());

  REQUIRE(result.contains("matches"));
  CHECK(result["count"].get<int>() == 1);
  CHECK(result["truncated"] == true);
}

TEST_CASE("code_tools: ast_search runs tree-sitter queries")
{
  mistral::tools::code_tools tools;
  mistral::tools::sample_file sample;

  auto result = mistral::tools::run_ast_search(
    tools, "(function_definition) @fn", sample.path.string());

  REQUIRE(result.contains("matches"));
  REQUIRE(result["count"].get<int>() == 1);
  REQUIRE(result["matches"][0]["captures"].size() == 1);
  CHECK(result["matches"][0]["captures"][0]["name"] == "fn");
  CHECK(result["matches"][0]["captures"][0]["line"].get<int>() == 7);
  CHECK(result["matches"][0]["captures"][0]["text"].get<std::string>() ==
    "int main() { return 0; }");
  CHECK(result["matches"][0]["line_start"].get<int>() == 7);
  CHECK(result["matches"][0]["line_end"].get<int>() == 7);
}

TEST_CASE("code_tools: ast_search evaluates predicates")
{
  mistral::tools::code_tools tools;
  mistral::tools::sample_project project;

  // Directory search with an #eq? predicate.
  auto result = mistral::tools::run_ast_search(
    tools,
    "((call_expression function: (identifier) @fn) (#eq? @fn \"alpha\"))",
    project.root.string());

  REQUIRE(result.contains("matches"));
  REQUIRE(result["count"].get<int>() == 1);
  CHECK(result["matches"][0]["captures"][0]["text"].get<std::string>() == "alpha");
  CHECK(result["matches"][0]["line_start"].get<int>() == 2);

  // #match? predicate.
  result = mistral::tools::run_ast_search(
    tools,
    "((function_definition declarator: (function_declarator declarator: (identifier) @n)) "
    "(#match? @n \"^al\"))",
    project.root.string());

  REQUIRE(result["count"].get<int>() == 1);
  CHECK(result["matches"][0]["captures"][0]["text"].get<std::string>() == "alpha");

  // #not-eq? predicate.
  result = mistral::tools::run_ast_search(
    tools,
    "((function_definition declarator: (function_declarator declarator: (identifier) @n)) "
    "(#not-eq? @n \"alpha\"))",
    project.root.string());

  REQUIRE(result["count"].get<int>() == 1);
  CHECK(result["matches"][0]["captures"][0]["text"].get<std::string>() == "caller");
}

TEST_CASE("code_tools: ast_search reports query errors")
{
  mistral::tools::code_tools tools;
  mistral::tools::sample_file sample;

  // Unknown node type.
  auto result = mistral::tools::run_ast_search(
    tools, "(nonexistent_node) @x", sample.path.string());
  CHECK(result.contains("error"));
  CHECK(result["error"].get<std::string>().find("unknown node type") != std::string::npos);

  // Unsupported predicate.
  result = mistral::tools::run_ast_search(
    tools, "((identifier) @x (#foo? @x))", sample.path.string());
  CHECK(result.contains("error"));
  CHECK(result["error"].get<std::string>().find("unsupported predicate") != std::string::npos);

  // Query without captures.
  result = mistral::tools::run_ast_search(
    tools, "(function_definition)", sample.path.string());
  CHECK(result.contains("error"));
  CHECK(result["error"].get<std::string>().find("capture") != std::string::npos);

  // Empty query.
  result = mistral::tools::run_ast_search(tools, "", sample.path.string());
  CHECK(result.contains("error"));
}