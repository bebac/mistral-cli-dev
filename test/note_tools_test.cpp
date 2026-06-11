#include "doctest/doctest.h"
#include "tools/note_tools.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace
{
  struct temp_notes_dir
  {
    fs::path path;

    temp_notes_dir() {
      path = fs::temp_directory_path() / "mistral_notes_test";
      fs::remove_all(path);
      fs::create_directories(path);
    }

    ~temp_notes_dir() {
      fs::remove_all(path);
    }

    void write(const std::string& name, const std::string& content) {
      auto p = path / name;
      fs::create_directories(p.parent_path());
      std::ofstream f(p);
      f << content;
    }
  };
}

// ---- Frontmatter parsing ----

TEST_CASE("parse_frontmatter: title and tags")
{
  auto result = mistral::tools::parse_frontmatter(
    "---\n"
    "title: My Note\n"
    "tags: [cpp, cmake, build]\n"
    "---\n"
    "# Content here\n"
  );

  CHECK(result.front.title == "My Note");
  REQUIRE(result.front.tags.size() == 3);
  CHECK(result.front.tags[0] == "cpp");
  CHECK(result.front.tags[1] == "cmake");
  CHECK(result.front.tags[2] == "build");
  CHECK(result.body == "# Content here\n");
}

TEST_CASE("parse_frontmatter: no frontmatter")
{
  auto result = mistral::tools::parse_frontmatter("Just plain content.\n");
  CHECK(result.front.title.empty());
  CHECK(result.front.tags.empty());
  CHECK(result.body == "Just plain content.\n");
}

TEST_CASE("parse_frontmatter: title only")
{
  auto result = mistral::tools::parse_frontmatter(
    "---\n"
    "title: Quick Note\n"
    "---\n"
    "Body text.\n"
  );

  CHECK(result.front.title == "Quick Note");
  CHECK(result.front.tags.empty());
  CHECK(result.body == "Body text.\n");
}

TEST_CASE("parse_frontmatter: tags without brackets")
{
  auto result = mistral::tools::parse_frontmatter(
    "---\n"
    "tags: alpha, beta\n"
    "---\n"
    "body\n"
  );

  REQUIRE(result.front.tags.size() == 2);
  CHECK(result.front.tags[0] == "alpha");
  CHECK(result.front.tags[1] == "beta");
}

// ---- Note CRUD ----

TEST_CASE("note_tools: write and read a note")
{
  temp_notes_dir tmp;
  mistral::tools::note_tools notes(tmp.path.string());

  auto w = notes.write_note("test-note", "---\ntitle: Test\ntags: [demo]\n---\nHello world.\n");
  CHECK(w["status"] == "ok");

  auto r = notes.read_note("test-note");
  CHECK(r["title"] == "Test");
  CHECK(r["tags"].size() == 1);
  CHECK(r.contains("content"));
  CHECK(r["content"].get<std::string>().find("Hello world.") != std::string::npos);
}

TEST_CASE("note_tools: write creates subdirectories")
{
  temp_notes_dir tmp;
  mistral::tools::note_tools notes(tmp.path.string());

  auto w = notes.write_note("projects/deep/nested", "content");
  CHECK(w["status"] == "ok");
  CHECK(fs::exists(tmp.path / "projects" / "deep" / "nested.md"));
}

TEST_CASE("note_tools: read nonexistent note")
{
  temp_notes_dir tmp;
  mistral::tools::note_tools notes(tmp.path.string());

  auto r = notes.read_note("no-such-note");
  CHECK(r.contains("error"));
}

TEST_CASE("note_tools: list notes with titles and tags")
{
  temp_notes_dir tmp;
  tmp.write("alpha.md", "---\ntitle: Alpha Note\ntags: [a, b]\n---\nbody\n");
  tmp.write("sub/beta.md", "---\ntitle: Beta\n---\nbeta body\n");

  mistral::tools::note_tools notes(tmp.path.string());
  auto listing = notes.list_notes("");

  CHECK(listing.find("alpha.md") != std::string::npos);
  CHECK(listing.find("Alpha Note") != std::string::npos);
  CHECK(listing.find("beta.md") != std::string::npos);
}

TEST_CASE("note_tools: list subfolder")
{
  temp_notes_dir tmp;
  tmp.write("top.md", "top level");
  tmp.write("sub/deep.md", "nested");

  mistral::tools::note_tools notes(tmp.path.string());
  auto listing = notes.list_notes("sub");

  CHECK(listing.find("deep.md") != std::string::npos);
  // top.md should not appear (it's outside the subfolder).
}

// ---- Search ----

TEST_CASE("note_tools: search by text")
{
  temp_notes_dir tmp;
  tmp.write("a.md", "---\ntitle: First\n---\nThe quick brown fox.\n");
  tmp.write("b.md", "---\ntitle: Second\n---\nThe lazy dog.\n");

  mistral::tools::note_tools notes(tmp.path.string());
  auto results = notes.search_notes("fox", "");

  REQUIRE(results.is_array());
  REQUIRE(results.size() == 1);
  CHECK(results[0]["title"] == "First");
  CHECK(results[0]["matches"].size() >= 1);
}

TEST_CASE("note_tools: search by tag")
{
  temp_notes_dir tmp;
  tmp.write("cpp.md", "---\ntitle: C++ Stuff\ntags: [cpp, dev]\n---\nbody\n");
  tmp.write("rust.md", "---\ntitle: Rust Stuff\ntags: [rust, dev]\n---\nbody\n");

  mistral::tools::note_tools notes(tmp.path.string());

  auto by_cpp = notes.search_notes("", "cpp");
  REQUIRE(by_cpp.size() == 1);
  CHECK(by_cpp[0]["title"] == "C++ Stuff");

  auto by_dev = notes.search_notes("", "dev");
  CHECK(by_dev.size() == 2);
}

TEST_CASE("note_tools: search by text + tag combined")
{
  temp_notes_dir tmp;
  tmp.write("a.md", "---\ntags: [web]\n---\nusing React framework\n");
  tmp.write("b.md", "---\ntags: [web]\n---\nusing Vue framework\n");
  tmp.write("c.md", "---\ntags: [backend]\n---\nusing React Native\n");

  mistral::tools::note_tools notes(tmp.path.string());
  auto results = notes.search_notes("react", "web");

  REQUIRE(results.size() == 1);
  CHECK(results[0]["path"] == "a.md");
}

TEST_CASE("note_tools: search case insensitive")
{
  temp_notes_dir tmp;
  tmp.write("note.md", "---\ntags: [CMake]\n---\nUsing CMAKE_BUILD_TYPE.\n");

  mistral::tools::note_tools notes(tmp.path.string());

  auto by_tag = notes.search_notes("", "cmake");
  CHECK(by_tag.size() == 1);

  auto by_text = notes.search_notes("cmake_build", "");
  CHECK(by_text.size() == 1);
}

// ---- Include resolution in read_note ----

TEST_CASE("note_tools: read resolves includes")
{
  temp_notes_dir tmp;
  tmp.write("_partials/sig.md", "-- signed by Bot");
  tmp.write("doc.md", "---\ntitle: Doc\n---\nContent.\n{{> _partials/sig}}\n");

  mistral::tools::note_tools notes(tmp.path.string());
  auto r = notes.read_note("doc");

  auto content = r["content"].get<std::string>();
  CHECK(content.find("-- signed by Bot") != std::string::npos);
  CHECK(content.find("Content.") != std::string::npos);
}

TEST_CASE("note_tools: write preserves include directives literally")
{
  temp_notes_dir tmp;
  mistral::tools::note_tools notes(tmp.path.string());

  notes.write_note("has-include", "Use this:\n{{> _partials/snippet}}\nDone.\n");

  // Read the raw file — include directive should be preserved.
  std::ifstream f(tmp.path / "has-include.md");
  std::ostringstream ss;
  ss << f.rdbuf();
  CHECK(ss.str().find("{{> _partials/snippet}}") != std::string::npos);
}
