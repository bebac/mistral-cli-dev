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