#include <doctest/doctest.h>
#include "common/string_utils.h"

#include <string>
#include <vector>

TEST_SUITE_BEGIN("string_utils");

// ---- trim tests ----

TEST_CASE("trim removes leading and trailing whitespace")
{
  using namespace mistral::utils;

  CHECK(trim("  hello  ") == "hello");
  CHECK(trim("hello") == "hello");
  CHECK(trim("  hello") == "hello");
  CHECK(trim("hello  ") == "hello");
}

TEST_CASE("trim handles different whitespace characters")
{
  using namespace mistral::utils;

  CHECK(trim("\t\thello\t") == "hello");
  CHECK(trim(" \t hello \t ") == "hello");
  CHECK(trim("\n\rhello\n\r") == "hello");
}

TEST_CASE("trim handles empty and whitespace-only strings")
{
  using namespace mistral::utils;

  CHECK(trim("") == "");
  CHECK(trim("   ") == "");
  CHECK(trim("\t\t") == "");
  CHECK(trim(" \t\n\r ") == "");
}

TEST_CASE("trim preserves internal whitespace")
{
  using namespace mistral::utils;

  CHECK(trim("hello world") == "hello world");
  CHECK(trim("  hello   world  ") == "hello   world");
}

// ---- to_lower tests ----

TEST_CASE("to_lower converts uppercase to lowercase")
{
  using namespace mistral::utils;

  CHECK(to_lower("HELLO") == "hello");
  CHECK(to_lower("Hello") == "hello");
  CHECK(to_lower("HELLO WORLD") == "hello world");
}

TEST_CASE("to_lower handles mixed case")
{
  using namespace mistral::utils;

  CHECK(to_lower("HeLLo WoRLd") == "hello world");
  CHECK(to_lower("MiXeD cAsE") == "mixed case");
}

TEST_CASE("to_lower handles empty and special characters")
{
  using namespace mistral::utils;

  CHECK(to_lower("") == "");
  CHECK(to_lower("123") == "123");
  CHECK(to_lower("!@#$%") == "!@#$%");
  CHECK(to_lower("Hello!@#") == "hello!@#");
}

// ---- starts_with tests ----

TEST_CASE("starts_with detects prefix at beginning")
{
  using namespace mistral::utils;

  CHECK(starts_with("hello world", "hello"));
  CHECK(starts_with("hello", "hello"));
  CHECK(starts_with("hello world", "hell"));
}

TEST_CASE("starts_with returns false for non-prefix")
{
  using namespace mistral::utils;

  CHECK(!starts_with("hello world", "world"));
  CHECK(!starts_with("hello", "hello world"));
  CHECK(!starts_with("world", "hello"));
}

TEST_CASE("starts_with handles empty strings")
{
  using namespace mistral::utils;

  CHECK(starts_with("hello", ""));
  CHECK(starts_with("", ""));
  CHECK(!starts_with("", "hello"));
}

TEST_CASE("starts_with is case sensitive")
{
  using namespace mistral::utils;

  CHECK(!starts_with("Hello", "hello"));
  CHECK(!starts_with("hello", "Hello"));
}

// ---- ends_with tests ----

TEST_CASE("ends_with detects suffix at end")
{
  using namespace mistral::utils;

  CHECK(ends_with("hello world", "world"));
  CHECK(ends_with("hello", "hello"));
  CHECK(ends_with("hello world", "orld"));
}

TEST_CASE("ends_with returns false for non-suffix")
{
  using namespace mistral::utils;

  CHECK(!ends_with("hello world", "hello"));
  CHECK(!ends_with("hello", "hello world"));
  CHECK(!ends_with("world", "hello"));
}

TEST_CASE("ends_with handles empty strings")
{
  using namespace mistral::utils;

  CHECK(ends_with("hello", ""));
  CHECK(ends_with("", ""));
  CHECK(!ends_with("", "hello"));
}

TEST_CASE("ends_with is case sensitive")
{
  using namespace mistral::utils;

  CHECK(!ends_with("World", "world"));
  CHECK(!ends_with("world", "World"));
}

// ---- split tests ----

TEST_CASE("split divides string by delimiter")
{
  using namespace mistral::utils;

  auto result = split("a,b,c", ',');
  CHECK(result == std::vector<std::string>{{"a", "b", "c"}});
}

TEST_CASE("split handles multiple consecutive delimiters")
{
  using namespace mistral::utils;

  auto result = split("a,,b,c", ',');
  CHECK(result == std::vector<std::string>{{"a", "b", "c"}});
}

TEST_CASE("split trims whitespace from tokens")
{
  using namespace mistral::utils;

  auto result = split("a, b , c", ',');
  CHECK(result == std::vector<std::string>{{"a", "b", "c"}});
}

TEST_CASE("split handles leading and trailing delimiters")
{
  using namespace mistral::utils;

  auto result = split(",a,b,c,", ',');
  CHECK(result == std::vector<std::string>{{"a", "b", "c"}});
}

TEST_CASE("split handles empty string")
{
  using namespace mistral::utils;

  auto result = split("", ',');
  CHECK(result.empty());
}

TEST_CASE("split handles string with only delimiters")
{
  using namespace mistral::utils;

  auto result = split(",,,", ',');
  CHECK(result.empty());
}

// ---- split_comma tests ----

TEST_CASE("split_comma splits by comma")
{
  using namespace mistral::utils;

  auto result = split_comma("a,b,c");
  CHECK(result == std::vector<std::string>{{"a", "b", "c"}});
}

TEST_CASE("split_comma handles spaces around commas")
{
  using namespace mistral::utils;

  auto result = split_comma("a, b , c");
  CHECK(result == std::vector<std::string>{{"a", "b", "c"}});
}

TEST_CASE("split_comma handles empty string")
{
  using namespace mistral::utils;

  auto result = split_comma("");
  CHECK(result.empty());
}

// ---- escape_regex tests ----

TEST_CASE("escape_regex escapes special characters")
{
  using namespace mistral::utils;

  CHECK(escape_regex(".") == "\\.");
  CHECK(escape_regex("*") == "\\*");
  CHECK(escape_regex("+") == "\\+");
  CHECK(escape_regex("?") == "\\?");
  CHECK(escape_regex("^") == "\\^");
  CHECK(escape_regex("$") == "\\$");
  CHECK(escape_regex("(") == "\\(");
  CHECK(escape_regex(")") == "\\)");
  CHECK(escape_regex("[") == "\\[");
  CHECK(escape_regex("]") == "\\]");
  CHECK(escape_regex("{") == "\\{");
  CHECK(escape_regex("}") == "\\}");
  CHECK(escape_regex("|") == "\\|");
  CHECK(escape_regex("\\") == "\\\\");
}

TEST_CASE("escape_regex handles multiple special characters")
{
  using namespace mistral::utils;

  CHECK(escape_regex(".*+") == "\\.\\*\\+");
  CHECK(escape_regex("a.b*c") == "a\\.b\\*c");
}

TEST_CASE("escape_regex leaves non-special characters unchanged")
{
  using namespace mistral::utils;

  CHECK(escape_regex("hello") == "hello");
  CHECK(escape_regex("abc123") == "abc123");
}

TEST_CASE("escape_regex handles empty string")
{
  using namespace mistral::utils;

  CHECK(escape_regex("") == "");
}

// ---- glob_to_regex tests ----

TEST_CASE("glob_to_regex converts simple patterns")
{
  using namespace mistral::utils;

  CHECK(glob_to_regex("test") == "^test$");
  CHECK(glob_to_regex("file.txt") == "^file\\.txt$");
}

TEST_CASE("glob_to_regex handles wildcard patterns")
{
  using namespace mistral::utils;

  CHECK(glob_to_regex("*") == "^[^/]*$");
  CHECK(glob_to_regex("*.txt") == "^[^/]*\\.txt$");
}

TEST_CASE("glob_to_regex handles double star patterns")
{
  using namespace mistral::utils;

  CHECK(glob_to_regex("**") == "^.*$");
  CHECK(glob_to_regex("**/*.txt") == "^.*/[^/]*\\.txt$");
}

TEST_CASE("glob_to_regex handles question mark patterns")
{
  using namespace mistral::utils;

  CHECK(glob_to_regex("file?.txt") == "^file.\\.txt$");
}

TEST_CASE("glob_to_regex handles complex patterns")
{
  using namespace mistral::utils;

  CHECK(glob_to_regex("src/**/*.cpp") == "^src/.*/[^/]*\\.cpp$");
  CHECK(glob_to_regex("test_*.h") == "^test_[^/]*\\.h$");
}

TEST_CASE("glob_to_regex handles empty pattern")
{
  using namespace mistral::utils;

  CHECK(glob_to_regex("") == "^$");
}

TEST_SUITE_END();