#include "tools/patchset_tools.h"
#include "tools/patchset_store.h"
#include "agent/edit_types.h"
#include "doctest/doctest.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace mistral::agent
{
  namespace
  {
    // In-memory mock patchset_store for testing
    class mock_patchset_store : public patchset_store
    {
    public:
      void store_patchset(const std::string& patchset_id, const std::vector<file_edit_t>& edits) override
      {
        stored_ = patchset_t{patchset_id, edits};
      }

      std::optional<patchset_t> get_patchset(const std::string& patchset_id) const override
      {
        if (stored_ && stored_->patchset_id == patchset_id)
        {
          return stored_;
        }
        return std::nullopt;
      }

      void remove_patchset() override
      {
        stored_.reset();
      }

      bool has_patchset() const
      {
        return stored_.has_value();
      }

    private:
      std::optional<patchset_t> stored_;
    };

    // RAII helper for temporary files
    class temp_file
    {
    public:
      explicit temp_file(const std::string& content = "")
      {
        path_ = "patchset_test_" + std::to_string(next_id_++) + ".tmp";
        if (!content.empty())
        {
          std::ofstream out(path_);
          out << content;
        }
      }

      ~temp_file()
      {
        if (std::filesystem::exists(path_))
        {
          std::filesystem::remove(path_);
        }
      }

      const std::string& path() const { return path_; }

      std::string read() const
      {
        std::ifstream in(path_);
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
      }

      void write(const std::string& content)
      {
        std::ofstream out(path_);
        out << content;
      }

      bool exists() const
      {
        return std::filesystem::exists(path_);
      }

    private:
      std::string path_;
      static inline int next_id_ = 0;
    };

    // Helper to create a simple edit
    file_edit_t make_edit(
      const std::string& file_path,
      const std::string& search_text,
      const std::string& replace_text,
      size_t patch_number = 1)
    {
      file_edit_t edit;
      edit.file_path = file_path;
      edit.search_text = search_text;
      edit.replace_text = replace_text;
      edit.patch_number = patch_number;
      return edit;
    }
  }

  TEST_SUITE_BEGIN("patchset_tools");

  // ---- patchset_get ----

  TEST_CASE("patchset_get - existing patchset")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f("hello world");
    auto edit = make_edit(f.path(), "hello", "goodbye", 1);
    store.store_patchset("test-id", {edit});

    auto result = tools.patchset_get("test-id");
    CHECK(result["patchset_id"] == "test-id");
    CHECK(result["edit_count"] == 1);
    CHECK(result["edits"][0]["patch_number"] == 1);
    CHECK(result["edits"][0]["file_path"] == f.path());
    CHECK(result["edits"][0]["search_text"] == "hello");
    CHECK(result["edits"][0]["replace_text"] == "goodbye");
  }

  TEST_CASE("patchset_get - non-existing patchset")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    auto result = tools.patchset_get("nonexistent");
    CHECK(result["status"] == "error");
    CHECK(result["error_type"] == "patchset_not_found");
    CHECK(result["error"].get<std::string>().find("nonexistent") != std::string::npos);
  }

  TEST_CASE("patchset_get - multiple edits")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f1("aaa");
    temp_file f2("bbb");
    auto edit1 = make_edit(f1.path(), "a", "x", 1);
    auto edit2 = make_edit(f2.path(), "b", "y", 2);
    store.store_patchset("multi", {edit1, edit2});

    auto result = tools.patchset_get("multi");
    CHECK(result["edit_count"] == 2);
    CHECK(result["edits"][0]["patch_number"] == 1);
    CHECK(result["edits"][1]["patch_number"] == 2);
  }

  // ---- patchset_validate ----

  TEST_CASE("patchset_validate - all valid")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f("hello world");
    auto edit = make_edit(f.path(), "hello", "goodbye", 1);
    store.store_patchset("valid", {edit});

    auto result = tools.patchset_validate("valid");
    CHECK(result["patchset_id"] == "valid");
    CHECK(result["all_valid"] == true);
    CHECK(result["fixable_error_count"] == 0);
    CHECK(result["validations"].size() == 1);
    CHECK(result["validations"][0]["valid"] == true);
  }

  TEST_CASE("patchset_validate - search_not_found (fixable)")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f("hello world");
    auto edit = make_edit(f.path(), "nonexistent", "replacement", 1);
    store.store_patchset("invalid", {edit});

    auto result = tools.patchset_validate("invalid");
    CHECK(result["all_valid"] == false);
    CHECK(result["fixable_error_count"] == 1);
    CHECK(result["validations"][0]["valid"] == false);
    CHECK(result["validations"][0]["error_type"] == "search_not_found");
    CHECK(result["validations"][0]["is_fixable"] == true);
  }

  TEST_CASE("patchset_validate - multiple_matches (fixable)")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f("foo foo bar");
    auto edit = make_edit(f.path(), "foo", "baz", 1);
    store.store_patchset("multi", {edit});

    auto result = tools.patchset_validate("multi");
    CHECK(result["all_valid"] == false);
    CHECK(result["fixable_error_count"] == 1);
    CHECK(result["validations"][0]["error_type"] == "multiple_matches");
    CHECK(result["validations"][0]["match_count"] == 2);
    CHECK(result["validations"][0]["is_fixable"] == true);
  }

  TEST_CASE("patchset_validate - file_not_found (non-fixable)")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    auto edit = make_edit("does_not_exist_file.tmp", "search", "replace", 1);
    store.store_patchset("nofile", {edit});

    auto result = tools.patchset_validate("nofile");
    CHECK(result["all_valid"] == false);
    CHECK(result["fixable_error_count"] == 0);
    CHECK(result["validations"][0]["error_type"] == "file_not_found");
    CHECK(result["validations"][0]["is_fixable"] == false);
  }

  TEST_CASE("patchset_validate - non-existing patchset")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    auto result = tools.patchset_validate("nonexistent");
    CHECK(result["status"] == "error");
    CHECK(result["error_type"] == "patchset_not_found");
  }

  TEST_CASE("patchset_validate - mixed valid and invalid")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f1("hello world");
    temp_file f2("foo foo bar");
    auto edit1 = make_edit(f1.path(), "hello", "goodbye", 1);
    auto edit2 = make_edit(f2.path(), "foo", "baz", 2);
    store.store_patchset("mixed", {edit1, edit2});

    auto result = tools.patchset_validate("mixed");
    CHECK(result["all_valid"] == false);
    CHECK(result["fixable_error_count"] == 1);
    CHECK(result["validations"].size() == 2);
    CHECK(result["validations"][0]["valid"] == true);
    CHECK(result["validations"][1]["valid"] == false);
    CHECK(result["validations"][1]["error_type"] == "multiple_matches");
  }

  // ---- patchset_apply ----

  TEST_CASE("patchset_apply - valid regular edit")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f("hello world");
    auto edit = make_edit(f.path(), "hello", "goodbye", 1);
    store.store_patchset("apply1", {edit});

    auto result = tools.patchset_apply("apply1");
    CHECK(result["status"] == "success");
    CHECK(result["applied"] == 1);
    CHECK(result["failed"] == 0);
    CHECK(result["reviewed"] == false);
    CHECK(result["results"][0]["status"] == "applied");
    CHECK(f.read() == "goodbye world");
  }

  TEST_CASE("patchset_apply - new file creation")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    std::string new_file = "patchset_new_file_test.tmp";
    // Ensure it doesn't exist
    if (std::filesystem::exists(new_file))
    {
      std::filesystem::remove(new_file);
    }

    auto edit = make_edit(new_file, "", "new file content", 1);
    store.store_patchset("create", {edit});

    auto result = tools.patchset_apply("create");
    CHECK(result["status"] == "success");
    CHECK(result["applied"] == 1);
    CHECK(result["failed"] == 0);

    std::ifstream in(new_file);
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    CHECK(content == "new file content");

    std::filesystem::remove(new_file);
  }

  TEST_CASE("patchset_apply - file overwrite")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f("old content");
    auto edit = make_edit(f.path(), ".", "completely new content", 1);
    store.store_patchset("overwrite", {edit});

    auto result = tools.patchset_apply("overwrite");
    CHECK(result["status"] == "success");
    CHECK(result["applied"] == 1);
    CHECK(result["failed"] == 0);
    CHECK(f.read() == "completely new content");
  }

  TEST_CASE("patchset_apply - non-existing patchset")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    auto result = tools.patchset_apply("nonexistent");
    CHECK(result["status"] == "error");
    CHECK(result["error_type"] == "patchset_not_found");
  }

  TEST_CASE("patchset_apply - fixable error triggers automatic correction")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f("hello world");
    auto edit = make_edit(f.path(), "nonexistent", "replacement", 1);
    store.store_patchset("fixable", {edit});

    auto result = tools.patchset_apply("fixable");
    CHECK(result["status"] == "validation_failed");
    CHECK(result["review_attempted"] == true);
    CHECK(result["review_status"] == "not_implemented");
    CHECK(result["applied"] == 0);
    CHECK(result["errors"].size() == 1);
    CHECK(result["errors"][0]["error_type"] == "search_not_found");
  }

  TEST_CASE("patchset_apply - non-fixable error returns validation failure")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    auto edit = make_edit("does_not_exist_file.tmp", "search", "replace", 1);
    store.store_patchset("nonfixable", {edit});

    auto result = tools.patchset_apply("nonfixable");
    CHECK(result["status"] == "validation_failed");
    CHECK(result["review_attempted"] == false);
    CHECK(result["applied"] == 0);
    CHECK(result["errors"].size() == 1);
    CHECK(result["errors"][0]["error_type"] == "file_not_found");
  }

  TEST_CASE("patchset_apply - multiple edits, partial failure")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f1("hello world");
    auto edit1 = make_edit(f1.path(), "hello", "goodbye", 1);
    // This edit references a file that doesn't exist - but validation
    // will catch it as file_not_found (non-fixable), so apply won't run.
    auto edit2 = make_edit("does_not_exist_file.tmp", "search", "replace", 2);
    store.store_patchset("partial", {edit1, edit2});

    auto result = tools.patchset_apply("partial");
    // Since there are non-fixable errors and no fixable errors,
    // it returns validation failure
    CHECK(result["status"] == "validation_failed");
    CHECK(result["review_attempted"] == false);
  }

  // ---- patchset_update ----

  TEST_CASE("patchset_update - update search_text only")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f("hello world");
    auto edit = make_edit(f.path(), "hello", "goodbye", 1);
    store.store_patchset("update1", {edit});

    auto result = tools.patchset_update("update1", 1, f.path(),
      std::optional<std::string>("hello"),
      std::nullopt);
    CHECK(result["status"] == "success");
    CHECK(result["patchset_id"] == "update1");
    CHECK(result["patch_number"] == 1);

    // Verify the update was stored
    auto get_result = tools.patchset_get("update1");
    CHECK(get_result["edits"][0]["search_text"] == "hello");
    CHECK(get_result["edits"][0]["replace_text"] == "goodbye");
  }

  TEST_CASE("patchset_update - update replace_text only")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f("hello world");
    auto edit = make_edit(f.path(), "hello", "goodbye", 1);
    store.store_patchset("update2", {edit});

    auto result = tools.patchset_update("update2", 1, f.path(),
      std::nullopt,
      std::optional<std::string>("bye"));
    CHECK(result["status"] == "success");

    auto get_result = tools.patchset_get("update2");
    CHECK(get_result["edits"][0]["search_text"] == "hello");
    CHECK(get_result["edits"][0]["replace_text"] == "bye");
  }

  TEST_CASE("patchset_update - update both fields")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f("hello world");
    auto edit = make_edit(f.path(), "hello", "goodbye", 1);
    store.store_patchset("update3", {edit});

    auto result = tools.patchset_update("update3", 1, f.path(),
      std::optional<std::string>("hi"),
      std::optional<std::string>("bye"));
    CHECK(result["status"] == "success");

    auto get_result = tools.patchset_get("update3");
    CHECK(get_result["edits"][0]["search_text"] == "hi");
    CHECK(get_result["edits"][0]["replace_text"] == "bye");
  }

  TEST_CASE("patchset_update - non-existing patch_number")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f("hello world");
    auto edit = make_edit(f.path(), "hello", "goodbye", 1);
    store.store_patchset("update4", {edit});

    auto result = tools.patchset_update("update4", 99, f.path(),
      std::optional<std::string>("new"),
      std::nullopt);
    CHECK(result["status"] == "error");
    CHECK(result["error_type"] == "edit_not_found");
    CHECK(result["error"].get<std::string>().find("99") != std::string::npos);
  }

  TEST_CASE("patchset_update - non-existing patchset")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    auto result = tools.patchset_update("nonexistent", 1, "file.tmp",
      std::optional<std::string>("new"),
      std::nullopt);
    CHECK(result["status"] == "error");
    CHECK(result["error_type"] == "patchset_not_found");
  }

  // ---- patchset_clear ----

  TEST_CASE("patchset_clear - clears stored patchset")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f("hello world");
    auto edit = make_edit(f.path(), "hello", "goodbye", 1);
    store.store_patchset("clear1", {edit});
    CHECK(store.has_patchset());

    auto result = tools.patchset_clear();
    CHECK(result["status"] == "success");
    CHECK(!store.has_patchset());
  }

  TEST_CASE("patchset_clear - get after clear returns not found")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f("hello world");
    auto edit = make_edit(f.path(), "hello", "goodbye", 1);
    store.store_patchset("clear2", {edit});

    tools.patchset_clear();

    auto result = tools.patchset_get("clear2");
    CHECK(result["status"] == "error");
    CHECK(result["error_type"] == "patchset_not_found");
  }

  TEST_SUITE_END();
} // namespace mistral::agent