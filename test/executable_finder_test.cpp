#include "common/executable_finder.h"
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

TEST_SUITE_BEGIN("executable_finder");

namespace
{
  // RAII temporary directory (mkdtemp for a unique name)
  class temp_dir
  {
  public:
    temp_dir()
    {
      std::string tmpl = (fs::temp_directory_path() / "mistral_find_exec_XXXXXX").string();
      std::vector<char> buf(tmpl.begin(), tmpl.end());
      buf.push_back('\0');
      char* result = ::mkdtemp(buf.data());
      REQUIRE(result != nullptr);
      path_ = result;
    }

    ~temp_dir()
    {
      fs::remove_all(path_);
    }

    const std::string& path() const
    {
      return path_;
    }

  private:
    std::string path_;
  };

  // RAII PATH override, restored on destruction so other tests are
  // unaffected
  class path_override
  {
  public:
    explicit path_override(const std::string& new_path)
    {
      const char* old = ::getenv("PATH");
      if (old)
        old_ = old;
      ::setenv("PATH", new_path.c_str(), 1);
    }

    ~path_override()
    {
      if (old_.empty())
      {
        ::unsetenv("PATH");
      }
      else
      {
        ::setenv("PATH", old_.c_str(), 1);
      }
    }

  private:
    std::string old_;
  };

  void write_file(const std::string& file, const std::string& content)
  {
    std::ofstream f(file);
    REQUIRE(f.is_open());
    f << content;
  }

  void make_executable(const std::string& file)
  {
    REQUIRE(::chmod(file.c_str(), 0755) == 0);
  }
} // namespace

TEST_CASE("find_executable returns the path for an existing executable file")
{
  temp_dir dir;
  std::string exe = dir.path() + "/mytool";
  write_file(exe, "#!/bin/sh\n");
  make_executable(exe);

  CHECK(mistral::utils::find_executable(exe) == exe);
}

TEST_CASE("find_executable rejects a non-executable file path")
{
  temp_dir dir;
  std::string file = dir.path() + "/plain.txt";
  write_file(file, "hello");
  REQUIRE(::chmod(file.c_str(), 0644) == 0);

  CHECK(mistral::utils::find_executable(file).empty());
}

TEST_CASE("find_executable rejects a missing path")
{
  temp_dir dir;
  CHECK(mistral::utils::find_executable(dir.path() + "/does_not_exist").empty());
}

TEST_CASE("find_executable searches PATH for a bare command name")
{
  temp_dir dir;
  std::string exe = dir.path() + "/mytool";
  write_file(exe, "#!/bin/sh\n");
  make_executable(exe);

  path_override po(dir.path());
  CHECK(mistral::utils::find_executable("mytool") == exe);
}

TEST_CASE("find_executable returns empty for a command not in PATH")
{
  temp_dir dir; // empty directory as the only PATH entry
  path_override po(dir.path());
  CHECK(mistral::utils::find_executable("definitely_not_on_path_xyz").empty());
}

TEST_CASE("find_executable ignores non-executable entries in PATH")
{
  temp_dir dir;
  std::string file = dir.path() + "/mytool";
  write_file(file, "data");
  REQUIRE(::chmod(file.c_str(), 0644) == 0);

  path_override po(dir.path());
  CHECK(mistral::utils::find_executable("mytool").empty());
}

TEST_CASE("find_executable handles empty entries in PATH")
{
  temp_dir dir;
  std::string exe = dir.path() + "/mytool";
  write_file(exe, "#!/bin/sh\n");
  make_executable(exe);

  // Empty PATH entries (including leading/trailing colons) are skipped
  path_override po(":" + dir.path() + "::");
  CHECK(mistral::utils::find_executable("mytool") == exe);
}

TEST_CASE("find_executable returns empty when PATH is not set")
{
  path_override po("");
  CHECK(mistral::utils::find_executable("anything_at_all").empty());
}

TEST_SUITE_END();
