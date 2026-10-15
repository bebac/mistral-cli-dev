#include "tools/patchset_tools.h"
#include "tools/patchset_store.h"
#include "tools/edit_types.h"
#include "doctest/doctest.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace mistral::tools
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

      void append_edits(const std::string& patchset_id, const std::vector<file_edit_t>& edits) override
      {
        if (stored_ && stored_->patchset_id == patchset_id && !stored_->applied)
        {
          // Number after the highest existing patch number (mirrors
          // agent_system::append_edits): patch numbers are stable
          // identifiers, so size-based numbering could collide after
          // a patchset_remove created gaps.
          size_t next_patch_number = 0;
          for (const auto& edit : stored_->edits)
          {
            next_patch_number = std::max(next_patch_number, edit.patch_number);
          }
          for (const auto& edit : edits)
          {
            auto numbered_edit = edit;
            numbered_edit.patch_number = ++next_patch_number;
            stored_->edits.push_back(numbered_edit);
          }
        }
        else
        {
          stored_ = patchset_t{patchset_id, edits};
        }
      }

      std::optional<patchset_t> get_patchset(const std::string& patchset_id) const override
      {
        if (stored_ && stored_->patchset_id == patchset_id)
        {
          return stored_;
        }
        return std::nullopt;
      }

      void remove_patchset(const std::string& patchset_id) override
      {
        if (stored_ && stored_->patchset_id == patchset_id)
        {
          stored_.reset();
        }
      }

      void mark_applied(const std::string& patchset_id) override
      {
        if (stored_ && stored_->patchset_id == patchset_id)
        {
          stored_->applied = true;
        }
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

    public:
      static int next_id_for_dir_tests()
      {
        return next_id_++;
      }
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
    tools::patchset_tools tools(store);

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
    tools::patchset_tools tools(store);

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

  TEST_CASE("patchset_apply - new file creation creates intermediate directories")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    std::string new_dir = "patchset_test_subdir_" +
      std::to_string(temp_file::next_id_for_dir_tests());
    std::string new_file = new_dir + "/nested/deep/file.tmp";

    // Ensure directory doesn't exist
    if (std::filesystem::exists(new_dir))
    {
      std::filesystem::remove_all(new_dir);
    }

    auto edit = make_edit(new_file, "", "nested content", 1);
    store.store_patchset("create-nested", {edit});

    auto result = tools.patchset_apply("create-nested");
    CHECK(result["status"] == "success");
    CHECK(result["applied"] == 1);
    CHECK(result["failed"] == 0);

    std::ifstream in(new_file);
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    CHECK(content == "nested content");

    // Cleanup
    std::filesystem::remove_all(new_dir);
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

  TEST_CASE("patchset_update - update file_path")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f("hello world");
    temp_file g("hello world");
    auto edit = make_edit(f.path(), "hello", "goodbye", 1);
    store.store_patchset("update5", {edit});

    auto result = tools.patchset_update("update5", 1, g.path(),
      std::nullopt,
      std::nullopt);
    CHECK(result["status"] == "success");
    CHECK(result["file_path"] == g.path());

    // Verify the new file path was stored, other fields unchanged
    auto get_result = tools.patchset_get("update5");
    CHECK(get_result["edits"][0]["file_path"] == g.path());
    CHECK(get_result["edits"][0]["search_text"] == "hello");
    CHECK(get_result["edits"][0]["replace_text"] == "goodbye");
  }

  TEST_CASE("patchset_update - empty file_path keeps existing")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f("hello world");
    auto edit = make_edit(f.path(), "hello", "goodbye", 1);
    store.store_patchset("update6", {edit});

    auto result = tools.patchset_update("update6", 1, "",
      std::nullopt,
      std::nullopt);
    CHECK(result["status"] == "success");

    auto get_result = tools.patchset_get("update6");
    CHECK(get_result["edits"][0]["file_path"] == f.path());
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

  // ---- patchset_remove ----

  TEST_CASE("patchset_remove - removes the edit and keeps other edits stable")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f1("hello world");
    temp_file f2("foo bar");
    auto edit1 = make_edit(f1.path(), "hello", "goodbye", 1);
    auto edit2 = make_edit(f2.path(), "foo", "baz", 2);
    auto edit3 = make_edit(f1.path(), "world", "planet", 3);
    store.store_patchset("remove1", {edit1, edit2, edit3});

    auto result = tools.patchset_remove("remove1", 2);
    CHECK(result["status"] == "success");
    CHECK(result["patchset_id"] == "remove1");
    CHECK(result["removed_patch_number"] == 2);
    CHECK(result["remaining_edit_count"] == 2);
    CHECK(result["remaining_patch_numbers"].size() == 2);
    CHECK(result["remaining_patch_numbers"][0] == 1);
    CHECK(result["remaining_patch_numbers"][1] == 3);

    // Verify the remaining edits are intact with stable patch numbers
    auto get_result = tools.patchset_get("remove1");
    CHECK(get_result["edit_count"] == 2);
    CHECK(get_result["edits"][0]["patch_number"] == 1);
    CHECK(get_result["edits"][0]["file_path"] == f1.path());
    CHECK(get_result["edits"][0]["search_text"] == "hello");
    CHECK(get_result["edits"][1]["patch_number"] == 3);
    CHECK(get_result["edits"][1]["file_path"] == f1.path());
    CHECK(get_result["edits"][1]["search_text"] == "world");
  }

  TEST_CASE("patchset_remove - non-existing patch_number")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f("hello world");
    auto edit = make_edit(f.path(), "hello", "goodbye", 1);
    store.store_patchset("remove2", {edit});

    auto result = tools.patchset_remove("remove2", 99);
    CHECK(result["status"] == "error");
    CHECK(result["error_type"] == "edit_not_found");
    CHECK(result["error"].get<std::string>().find("99") != std::string::npos);

    // Patchset is unchanged
    auto get_result = tools.patchset_get("remove2");
    CHECK(get_result["edit_count"] == 1);
  }

  TEST_CASE("patchset_remove - non-existing patchset")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    auto result = tools.patchset_remove("nonexistent", 1);
    CHECK(result["status"] == "error");
    CHECK(result["error_type"] == "patchset_not_found");
  }

  TEST_CASE("patchset_remove - already applied returns error")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f("hello world");
    auto edit = make_edit(f.path(), "hello", "goodbye", 1);
    store.store_patchset("remove3", {edit});

    // Apply the patchset first
    auto apply_result = tools.patchset_apply("remove3");
    CHECK(apply_result["status"] == "success");

    // Attempt to remove should fail with already_applied
    auto result = tools.patchset_remove("remove3", 1);
    CHECK(result["status"] == "error");
    CHECK(result["error_type"] == "already_applied");
  }

  TEST_CASE("patchset_remove - append after remove does not collide")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f1("hello world");
    temp_file f2("foo bar");
    temp_file f3("baz qux");
    auto edit1 = make_edit(f1.path(), "hello", "goodbye", 1);
    auto edit2 = make_edit(f2.path(), "foo", "baz", 2);
    auto edit3 = make_edit(f3.path(), "baz", "quux", 3);
    store.store_patchset("remove4", {edit1, edit2});

    // Remove patch 1: remaining patch number is 2, but size is 1.
    auto result = tools.patchset_remove("remove4", 1);
    CHECK(result["status"] == "success");

    // Appending must number the new edit 3 (max + 1), not 2 (size + 1),
    // which would collide with the existing patch number 2.
    store.append_edits("remove4", {edit3});

    auto retrieved = store.get_patchset("remove4");
    REQUIRE(retrieved.has_value());
    REQUIRE(retrieved->edits.size() == 2);
    CHECK(retrieved->edits[0].patch_number == 2);
    CHECK(retrieved->edits[0].file_path == f2.path());
    CHECK(retrieved->edits[1].patch_number == 3);
    CHECK(retrieved->edits[1].file_path == f3.path());
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

    auto result = tools.patchset_clear("clear1");
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

    tools.patchset_clear("clear2");

    auto result = tools.patchset_get("clear2");
    CHECK(result["status"] == "error");
    CHECK(result["error_type"] == "patchset_not_found");
  }

  TEST_CASE("patchset_clear - non-existing patchset")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    auto result = tools.patchset_clear("nonexistent");
    CHECK(result["status"] == "error");
    CHECK(result["error_type"] == "patchset_not_found");
  }

  // ---- patchset_add ----

  TEST_CASE("patchset_add - creates new patchset when ID is new")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f("hello world");
    auto result = tools.patchset_add("add1", f.path(), "hello", "goodbye");

    CHECK(result["status"] == "success");
    CHECK(result["patchset_id"] == "add1");
    CHECK(result["created"] == true);
    CHECK(result["patch_number"] == 1);
    CHECK(result["edit_count"] == 1);

    auto get_result = tools.patchset_get("add1");
    CHECK(get_result["edit_count"] == 1);
    CHECK(get_result["edits"][0]["file_path"] == f.path());
    CHECK(get_result["edits"][0]["search_text"] == "hello");
    CHECK(get_result["edits"][0]["replace_text"] == "goodbye");
  }

  TEST_CASE("patchset_add - appends to existing non-applied patchset")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f("hello world");
    temp_file g("foo bar");
    tools.patchset_add("add2", f.path(), "hello", "goodbye");
    auto result = tools.patchset_add("add2", g.path(), "foo", "baz");

    CHECK(result["status"] == "success");
    CHECK(result["created"] == false);
    CHECK(result["patch_number"] == 2);
    CHECK(result["edit_count"] == 2);
  }

  TEST_CASE("patchset_add - appends to text-emitted patchset")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f("hello world");
    temp_file g("foo bar");
    // Simulate a patchset created by emitted edit blocks
    store.store_patchset("add3", {make_edit(f.path(), "hello", "goodbye", 1)});

    auto result = tools.patchset_add("add3", g.path(), "foo", "baz");
    CHECK(result["status"] == "success");
    CHECK(result["created"] == false);
    CHECK(result["patch_number"] == 2);
    CHECK(result["edit_count"] == 2);
  }

  TEST_CASE("patchset_add - numbering continues after patchset_remove without collision")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f("hello world");
    temp_file g("foo bar");
    temp_file h("baz qux");
    tools.patchset_add("add4", f.path(), "hello", "goodbye"); // patch 1
    tools.patchset_add("add4", g.path(), "foo", "baz");       // patch 2
    CHECK(tools.patchset_remove("add4", 2)["status"] == "success");

    // Numbering is max(existing) + 1 (mirrors agent_system::append_edits):
    // the new edit takes the freed number 2, never colliding with a
    // remaining edit.
    auto result = tools.patchset_add("add4", h.path(), "baz", "quux");
    CHECK(result["status"] == "success");
    CHECK(result["patch_number"] == 2);
    CHECK(result["edit_count"] == 2);
  }

  TEST_CASE("patchset_add - empty search_text stores new-file edit")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    auto result = tools.patchset_add("add5", "new_dir/new_file.txt", "", "content");
    CHECK(result["status"] == "success");

    auto get_result = tools.patchset_get("add5");
    CHECK(get_result["edits"][0]["search_text"] == "");
    CHECK(get_result["edits"][0]["replace_text"] == "content");
  }

  TEST_CASE("patchset_add - already applied returns error")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f("hello world");
    tools.patchset_add("add6", f.path(), "hello", "goodbye");
    store.mark_applied("add6");

    auto result = tools.patchset_add("add6", f.path(), "hello", "again");
    CHECK(result["status"] == "error");
    CHECK(result["error_type"] == "already_applied");
  }

  TEST_CASE("patchset_add - empty file_path returns error")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    auto result = tools.patchset_add("add7", "", "hello", "goodbye");
    CHECK(result["status"] == "error");
    CHECK(result["error_type"] == "missing_file_path");
  }

  TEST_CASE("patchset_add - empty patchset_id returns error")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    auto result = tools.patchset_add("", "some/file.txt", "hello", "goodbye");
    CHECK(result["status"] == "error");
    CHECK(result["error_type"] == "missing_patchset_id");
  }

  // ---- patchset_apply already_applied guard ----

  TEST_CASE("patchset_apply - already applied returns error")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f("hello world");
    auto edit = make_edit(f.path(), "hello", "goodbye", 1);
    store.store_patchset("applied1", {edit});

    // First apply should succeed
    auto result1 = tools.patchset_apply("applied1");
    CHECK(result1["status"] == "success");
    CHECK(result1["applied"] == 1);

    // Second apply should fail with already_applied
    auto result2 = tools.patchset_apply("applied1");
    CHECK(result2["status"] == "error");
    CHECK(result2["error_type"] == "already_applied");
  }

  // ---- patchset_update already_applied guard ----

  TEST_CASE("patchset_update - already applied returns error")
  {
    mock_patchset_store store;
    patchset_tools tools(store);

    temp_file f("hello world");
    auto edit = make_edit(f.path(), "hello", "goodbye", 1);
    store.store_patchset("applied2", {edit});

    // Apply the patchset first
    auto apply_result = tools.patchset_apply("applied2");
    CHECK(apply_result["status"] == "success");

    // Attempt to update should fail with already_applied
    auto result = tools.patchset_update("applied2", 1, f.path(),
      std::optional<std::string>("new_search"),
      std::nullopt);
    CHECK(result["status"] == "error");
    CHECK(result["error_type"] == "already_applied");
  }

  // ---- append_edits ----

  TEST_CASE("append_edits - appends to existing non-applied patchset")
  {
    mock_patchset_store store;
    temp_file f1("hello world");
    temp_file f2("foo bar");
    auto edit1 = make_edit(f1.path(), "hello", "goodbye", 1);
    auto edit2 = make_edit(f2.path(), "foo", "baz", 2);

    store.store_patchset("append-test", {edit1});

    // Append second edit
    store.append_edits("append-test", {edit2});

    auto retrieved = store.get_patchset("append-test");
    REQUIRE(retrieved.has_value());
    CHECK(retrieved->edits.size() == 2);
    CHECK(retrieved->edits[0].patch_number == 1);
    CHECK(retrieved->edits[1].patch_number == 2);
    CHECK(retrieved->edits[1].file_path == f2.path());
  }

  TEST_CASE("append_edits - creates new patchset when ID does not match")
  {
    mock_patchset_store store;
    temp_file f1("hello world");
    temp_file f2("foo bar");
    auto edit1 = make_edit(f1.path(), "hello", "goodbye", 1);
    auto edit2 = make_edit(f2.path(), "foo", "baz", 2);

    store.store_patchset("original", {edit1});

    // Append with different ID should create new patchset
    store.append_edits("different-id", {edit2});

    // Original should be replaced
    CHECK(!store.get_patchset("original").has_value());
    auto retrieved = store.get_patchset("different-id");
    REQUIRE(retrieved.has_value());
    CHECK(retrieved->edits.size() == 1);
    CHECK(retrieved->edits[0].file_path == f2.path());
  }

  TEST_CASE("append_edits - does not append to applied patchset")
  {
    mock_patchset_store store;
    temp_file f1("hello world");
    temp_file f2("foo bar");
    auto edit1 = make_edit(f1.path(), "hello", "goodbye", 1);
    auto edit2 = make_edit(f2.path(), "foo", "baz", 2);

    store.store_patchset("applied-test", {edit1});
    store.mark_applied("applied-test");

    // Append should create new patchset since existing is applied
    store.append_edits("applied-test", {edit2});

    auto retrieved = store.get_patchset("applied-test");
    REQUIRE(retrieved.has_value());
    // Should have been overwritten, not appended
    CHECK(retrieved->edits.size() == 1);
    CHECK(retrieved->edits[0].file_path == f2.path());
    CHECK(!retrieved->applied);
  }

  TEST_CASE("append_edits - continues patch numbering correctly")
  {
    mock_patchset_store store;
    temp_file f1("a");
    temp_file f2("b");
    temp_file f3("c");

    auto edit1 = make_edit(f1.path(), "a", "x", 1);
    auto edit2 = make_edit(f2.path(), "b", "y", 2);
    auto edit3 = make_edit(f3.path(), "c", "z", 3);

    // Store with 2 edits
    store.store_patchset("numbering", {edit1, edit2});

    // Append 1 more edit (patch_number in the input doesn't matter,
    // append_edits should renumber)
    store.append_edits("numbering", {edit3});

    auto retrieved = store.get_patchset("numbering");
    REQUIRE(retrieved.has_value());
    CHECK(retrieved->edits.size() == 3);
    CHECK(retrieved->edits[0].patch_number == 1);
    CHECK(retrieved->edits[1].patch_number == 2);
    CHECK(retrieved->edits[2].patch_number == 3);
    CHECK(retrieved->edits[2].file_path == f3.path());
  }

  TEST_SUITE_END();
} // namespace mistral::tools
