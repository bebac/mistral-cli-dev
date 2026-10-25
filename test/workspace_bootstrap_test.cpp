#include "doctest/doctest.h"
#include "agent/workspace_bootstrap.h"
#include "common/embedded_resources.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
  // Helper: a throwaway workspace directory.
  struct temp_workspace
  {
    fs::path path;

    temp_workspace() {
      path = fs::temp_directory_path() / "mistral_bootstrap_test";
      fs::remove_all(path);
      fs::create_directories(path);
    }

    ~temp_workspace() {
      fs::remove_all(path);
    }

    void write(const std::string& name, const std::string& content) {
      std::ofstream f(path / name, std::ios::binary | std::ios::trunc);
      f << content;
    }

    std::string read(const std::string& name) {
      std::ifstream f(path / name, std::ios::binary);
      return std::string(std::istreambuf_iterator<char>(f),
                         std::istreambuf_iterator<char>());
    }

    json manifest() {
      std::ifstream f(path / "bootstrap.json");
      json m;
      f >> m;
      return m;
    }

    void set_manifest(const json& m) {
      write("bootstrap.json", m.dump(2));
    }
  };

  bool contains(const std::vector<std::string>& v, const std::string& needle) {
    for (const auto& s : v) {
      if (s.find(needle) != std::string::npos) return true;
    }
    return false;
  }

  std::string str(std::string_view sv) {
    return std::string(sv);
  }
}

TEST_CASE("fnv1a_64_hex: known vectors")
{
  CHECK(mistral::agent::fnv1a_64_hex("") == "cbf29ce484222325");
  CHECK(mistral::agent::fnv1a_64_hex("a") == "af63dc4c8601ec8c");
  CHECK(mistral::agent::fnv1a_64_hex("foobar") == "85944171f73967e8");
}

TEST_CASE("bootstrap: seeds all files into an empty workspace")
{
  temp_workspace ws;
  auto res = mistral::agent::bootstrap_workspace(ws.path);

  REQUIRE(res.ran);
  REQUIRE(res.created.size() == 7);
  CHECK(contains(res.created, "mistral-cli.json"));
  CHECK(contains(res.created, "policy.json"));
  CHECK(contains(res.created, "sys_prompt.md"));
  CHECK(contains(res.created, "sys_prompt_researcher.md"));
  CHECK(contains(res.created, "sys_prompt_xerj.md"));
  CHECK(contains(res.created, "sys_storybook_notes.md"));
  CHECK(contains(res.created, "sys_patchset_instructions.md"));

  CHECK(ws.read("mistral-cli.json") == str(mistral::embedded::config_json()));
  CHECK(ws.read("policy.json") == str(mistral::embedded::policy_json()));
  CHECK(ws.read("sys_prompt.md") == str(mistral::embedded::sys_prompt()));
  CHECK(ws.read("sys_prompt_researcher.md") ==
        str(mistral::embedded::sys_prompt_researcher()));
  CHECK(ws.read("sys_prompt_xerj.md") ==
        str(mistral::embedded::sys_prompt_xerj()));
  CHECK(ws.read("sys_storybook_notes.md") ==
        str(mistral::embedded::sys_storybook_notes()));
  CHECK(ws.read("sys_patchset_instructions.md") ==
        str(mistral::embedded::sys_patchset_instructions()));

  // Manifest records the managed file.
  const json m = ws.manifest();
  REQUIRE(m.contains("patchset_instructions"));
  CHECK(m["patchset_instructions"]["version"] ==
        mistral::embedded::patchset_instructions_version);
  CHECK(m["patchset_instructions"]["hash"] ==
        mistral::agent::fnv1a_64_hex(mistral::embedded::sys_patchset_instructions()));
}

TEST_CASE("bootstrap: never overwrites seeded files")
{
  temp_workspace ws;
  ws.write("sys_prompt.md", "my custom prompt");
  ws.write("sys_storybook_notes.md", "my notes");
  ws.write("policy.json", "{}");
  ws.write("mistral-cli.json", "{}");

  auto res = mistral::agent::bootstrap_workspace(ws.path);

  // sys_prompt_researcher.md + sys_prompt_xerj.md + the managed file
  CHECK(res.created.size() == 3);
  CHECK(ws.read("sys_prompt.md") == "my custom prompt");
  CHECK(ws.read("sys_storybook_notes.md") == "my notes");
  CHECK(ws.read("policy.json") == "{}");
  CHECK(ws.read("mistral-cli.json") == "{}");
}

TEST_CASE("bootstrap: updates unmodified stock managed file on version bump")
{
  temp_workspace ws;
  mistral::agent::bootstrap_workspace(ws.path);

  // Simulate a workspace bootstrapped by an older executable: same content
  // hash, lower recorded version.
  json m = ws.manifest();
  m["patchset_instructions"]["version"] = 0;
  ws.set_manifest(m);

  auto res = mistral::agent::bootstrap_workspace(ws.path);
  REQUIRE(res.updated.size() == 1);
  CHECK(contains(res.updated, "v0 -> v" +
              std::to_string(mistral::embedded::patchset_instructions_version)));
  CHECK(ws.manifest()["patchset_instructions"]["version"] ==
        mistral::embedded::patchset_instructions_version);
  CHECK(ws.read("sys_patchset_instructions.md") ==
        str(mistral::embedded::sys_patchset_instructions()));
}

TEST_CASE("bootstrap: no-op when managed file is current")
{
  temp_workspace ws;
  mistral::agent::bootstrap_workspace(ws.path);

  auto res = mistral::agent::bootstrap_workspace(ws.path);
  CHECK(res.created.empty());
  CHECK(res.updated.empty());
  CHECK(res.warnings.empty());
}

TEST_CASE("bootstrap: leaves locally modified managed file alone")
{
  temp_workspace ws;
  mistral::agent::bootstrap_workspace(ws.path);

  json m = ws.manifest();
  m["patchset_instructions"]["version"] = 0;
  ws.set_manifest(m);
  ws.write("sys_patchset_instructions.md", "my custom instructions");

  auto res = mistral::agent::bootstrap_workspace(ws.path);
  CHECK(res.updated.empty());
  REQUIRE(res.warnings.size() == 1);
  CHECK(contains(res.warnings, "modified locally"));
  CHECK(ws.read("sys_patchset_instructions.md") == "my custom instructions");
}

TEST_CASE("bootstrap: force_update overwrites a locally modified file")
{
  temp_workspace ws;
  mistral::agent::bootstrap_workspace(ws.path);
  ws.write("sys_patchset_instructions.md", "my custom instructions");

  mistral::agent::bootstrap_options opts;
  opts.force_update = true;
  auto res = mistral::agent::bootstrap_workspace(ws.path, opts);

  REQUIRE(res.updated.size() == 1);
  CHECK(contains(res.updated, "forced update"));
  CHECK(ws.read("sys_patchset_instructions.md") ==
        str(mistral::embedded::sys_patchset_instructions()));
  CHECK(ws.manifest()["patchset_instructions"]["hash"] ==
        mistral::agent::fnv1a_64_hex(mistral::embedded::sys_patchset_instructions()));
}

TEST_CASE("bootstrap: warns about untracked managed file")
{
  temp_workspace ws;
  ws.write("sys_patchset_instructions.md", "handcrafted");

  auto res = mistral::agent::bootstrap_workspace(ws.path);
  CHECK(res.created.size() == 6); // seeded files only
  REQUIRE(res.warnings.size() == 1);
  CHECK(contains(res.warnings, "not tracked"));
  CHECK(ws.read("sys_patchset_instructions.md") == "handcrafted");
}

TEST_CASE("bootstrap: disabled option is a no-op")
{
  temp_workspace ws;
  mistral::agent::bootstrap_options opts;
  opts.enabled = false;

  auto res = mistral::agent::bootstrap_workspace(ws.path, opts);
  CHECK(!res.ran);
  CHECK(res.created.empty());
  CHECK(fs::is_empty(ws.path));
}