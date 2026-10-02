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
  CHECK((*symbol)["kind"] == "function");
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