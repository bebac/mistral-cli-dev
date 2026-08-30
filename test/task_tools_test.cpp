#include "doctest/doctest.h"
#include "tools/task_tools.h"

#include <dripcore/scheduler.h>
#include <dripcore/task.h>
#include <dripcore/awaitable.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace
{
  struct temp_tasks_dir
  {
    fs::path path;

    temp_tasks_dir() {
      path = fs::temp_directory_path() / "mistral_tasks_test";
      fs::remove_all(path);
      fs::create_directories(path);
    }

    ~temp_tasks_dir() {
      fs::remove_all(path);
    }
  };

  // Helper to extract the value type from an awaitable
  template <typename T>
  struct awaitable_value {
    using type = T;
  };

  template <typename T>
  struct awaitable_value<dripcore::awaitable<T>> {
    using type = T;
  };

  template <typename T>
  using awaitable_value_t = typename awaitable_value<T>::type;

  template <typename T>
  class async_result {
  public:
    async_result() : completed_(false) {}
    void set(T value) { value_ = std::move(value); completed_ = true; }
    T get() const { return std::move(value_); }
    bool completed() const { return completed_; }
  private:
    T value_;
    bool completed_;
  };

  template <typename Func>
  auto run_async_task_op(Func&& func) {
    using func_result = decltype(func());
    using result_type = awaitable_value_t<func_result>;

    async_result<result_type> result_holder;

    class async_task_op : public dripcore::task
    {
    public:
      async_task_op(Func&& f, async_result<result_type>& holder)
        : func_(std::forward<Func>(f)), holder_(holder) {}

      dripcore::awaitable<> body() override {
        if constexpr (std::is_same_v<func_result, dripcore::awaitable<result_type>>) {
          holder_.set(co_await func_());
        } else {
          holder_.set(func_());
        }
        co_return;
      }

    private:
      Func func_;
      async_result<result_type>& holder_;
    };

    dripcore::scheduler scheduler;
    scheduler.spawn<async_task_op>(std::forward<Func>(func), result_holder);
    scheduler.run();

    if (result_holder.completed()) {
      return result_holder.get();
    }
    return result_type{};
  }
}

// ---- Create ----

TEST_CASE("task_tools: create task with defaults")
{
  temp_tasks_dir tmp;
  mistral::tools::task_tools tasks(tmp.path.string());

  auto r = run_async_task_op([&]() {
    return tasks.create_task("Implement feature X", "", "", {});
  });

  CHECK(r["id"] == "task-001");
  CHECK(r["title"] == "Implement feature X");
  CHECK(r["status"] == "todo");
  CHECK(r["priority"] == "medium");
  CHECK(r.contains("created_at"));
  CHECK(r.contains("updated_at"));
}

TEST_CASE("task_tools: create task with all fields")
{
  temp_tasks_dir tmp;
  mistral::tools::task_tools tasks(tmp.path.string());

  auto r = run_async_task_op([&]() {
    return tasks.create_task("Fix bug Y", "Detailed description here", "high", {"bug", "urgent"});
  });

  CHECK(r["id"] == "task-001");
  CHECK(r["title"] == "Fix bug Y");
  CHECK(r["description"] == "Detailed description here");
  CHECK(r["priority"] == "high");
  CHECK(r["tags"].size() == 2);
  CHECK(r["tags"][0] == "bug");
}

TEST_CASE("task_tools: create multiple tasks gets sequential IDs")
{
  temp_tasks_dir tmp;
  mistral::tools::task_tools tasks(tmp.path.string());

  auto r1 = run_async_task_op([&]() { return tasks.create_task("Task A", "", "", {}); });
  auto r2 = run_async_task_op([&]() { return tasks.create_task("Task B", "", "", {}); });
  auto r3 = run_async_task_op([&]() { return tasks.create_task("Task C", "", "", {}); });

  CHECK(r1["id"] == "task-001");
  CHECK(r2["id"] == "task-002");
  CHECK(r3["id"] == "task-003");
}

TEST_CASE("task_tools: create task with empty title fails")
{
  temp_tasks_dir tmp;
  mistral::tools::task_tools tasks(tmp.path.string());

  auto r = run_async_task_op([&]() { return tasks.create_task("", "", "", {}); });
  CHECK(r.contains("error"));
}

TEST_CASE("task_tools: create task with invalid priority fails")
{
  temp_tasks_dir tmp;
  mistral::tools::task_tools tasks(tmp.path.string());

  auto r = run_async_task_op([&]() { return tasks.create_task("Test", "", "super-high", {}); });
  CHECK(r.contains("error"));
}

// ---- List ----

TEST_CASE("task_tools: list all tasks")
{
  temp_tasks_dir tmp;
  mistral::tools::task_tools tasks(tmp.path.string());

  run_async_task_op([&]() { return tasks.create_task("Task A", "", "low", {}); });
  run_async_task_op([&]() { return tasks.create_task("Task B", "", "high", {"backend"}); });

  auto results = run_async_task_op([&]() { return tasks.list_tasks("", "", ""); });
  REQUIRE(results.is_array());
  CHECK(results.size() == 2);
}

TEST_CASE("task_tools: list tasks filtered by status")
{
  temp_tasks_dir tmp;
  mistral::tools::task_tools tasks(tmp.path.string());

  run_async_task_op([&]() { return tasks.create_task("Task A", "", "", {}); });
  auto t2 = run_async_task_op([&]() { return tasks.create_task("Task B", "", "", {}); });

  run_async_task_op([&]() {
    return tasks.update_task(t2["id"], "in-progress", "", "", "");
  });

  auto results = run_async_task_op([&]() { return tasks.list_tasks("in-progress", "", ""); });
  REQUIRE(results.size() == 1);
  CHECK(results[0]["title"] == "Task B");
}

TEST_CASE("task_tools: list tasks filtered by tag")
{
  temp_tasks_dir tmp;
  mistral::tools::task_tools tasks(tmp.path.string());

  run_async_task_op([&]() { return tasks.create_task("Task A", "", "", {"frontend"}); });
  run_async_task_op([&]() { return tasks.create_task("Task B", "", "", {"backend"}); });
  run_async_task_op([&]() { return tasks.create_task("Task C", "", "", {"frontend", "api"}); });

  auto results = run_async_task_op([&]() { return tasks.list_tasks("", "frontend", ""); });
  REQUIRE(results.size() == 2);
}

TEST_CASE("task_tools: list tasks filtered by priority")
{
  temp_tasks_dir tmp;
  mistral::tools::task_tools tasks(tmp.path.string());

  run_async_task_op([&]() { return tasks.create_task("Task A", "", "low", {}); });
  run_async_task_op([&]() { return tasks.create_task("Task B", "", "high", {}); });
  run_async_task_op([&]() { return tasks.create_task("Task C", "", "high", {}); });

  auto results = run_async_task_op([&]() { return tasks.list_tasks("", "", "high"); });
  REQUIRE(results.size() == 2);
}

TEST_CASE("task_tools: list with invalid status fails")
{
  temp_tasks_dir tmp;
  mistral::tools::task_tools tasks(tmp.path.string());

  auto r = run_async_task_op([&]() { return tasks.list_tasks("invalid", "", ""); });
  CHECK(r.contains("error"));
}

// ---- Get ----

TEST_CASE("task_tools: get existing task")
{
  temp_tasks_dir tmp;
  mistral::tools::task_tools tasks(tmp.path.string());

  auto created = run_async_task_op([&]() {
    return tasks.create_task("My Task", "Description", "high", {"tag1"});
  });

  auto r = run_async_task_op([&]() { return tasks.get_task(created["id"]); });
  CHECK(r["id"] == created["id"]);
  CHECK(r["title"] == "My Task");
  CHECK(r["description"] == "Description");
  CHECK(r["priority"] == "high");
}

TEST_CASE("task_tools: get nonexistent task")
{
  temp_tasks_dir tmp;
  mistral::tools::task_tools tasks(tmp.path.string());

  auto r = run_async_task_op([&]() { return tasks.get_task("task-999"); });
  CHECK(r.contains("error"));
}

// ---- Update ----

TEST_CASE("task_tools: update task status")
{
  temp_tasks_dir tmp;
  mistral::tools::task_tools tasks(tmp.path.string());

  auto created = run_async_task_op([&]() { return tasks.create_task("Task", "", "", {}); });

  auto r = run_async_task_op([&]() {
    return tasks.update_task(created["id"], "in-progress", "", "", "");
  });

  CHECK(r["status"] == "in-progress");
  CHECK(r["title"] == "Task");
}

TEST_CASE("task_tools: update task priority")
{
  temp_tasks_dir tmp;
  mistral::tools::task_tools tasks(tmp.path.string());

  auto created = run_async_task_op([&]() { return tasks.create_task("Task", "", "", {}); });

  auto r = run_async_task_op([&]() {
    return tasks.update_task(created["id"], "", "critical", "", "");
  });

  CHECK(r["priority"] == "critical");
}

TEST_CASE("task_tools: update task title and description")
{
  temp_tasks_dir tmp;
  mistral::tools::task_tools tasks(tmp.path.string());

  auto created = run_async_task_op([&]() { return tasks.create_task("Old Title", "Old Desc", "", {}); });

  auto r = run_async_task_op([&]() {
    return tasks.update_task(created["id"], "", "", "New Title", "New Desc");
  });

  CHECK(r["title"] == "New Title");
  CHECK(r["description"] == "New Desc");
}

TEST_CASE("task_tools: update nonexistent task fails")
{
  temp_tasks_dir tmp;
  mistral::tools::task_tools tasks(tmp.path.string());

  auto r = run_async_task_op([&]() { return tasks.update_task("task-999", "done", "", "", ""); });
  CHECK(r.contains("error"));
}

TEST_CASE("task_tools: update with invalid status fails")
{
  temp_tasks_dir tmp;
  mistral::tools::task_tools tasks(tmp.path.string());

  auto created = run_async_task_op([&]() { return tasks.create_task("Task", "", "", {}); });

  auto r = run_async_task_op([&]() {
    return tasks.update_task(created["id"], "invalid", "", "", "");
  });
  CHECK(r.contains("error"));
}

// ---- Search ----

TEST_CASE("task_tools: search by title")
{
  temp_tasks_dir tmp;
  mistral::tools::task_tools tasks(tmp.path.string());

  run_async_task_op([&]() { return tasks.create_task("Refactor agent system", "", "", {}); });
  run_async_task_op([&]() { return tasks.create_task("Fix HTTP bug", "", "", {}); });

  auto results = run_async_task_op([&]() { return tasks.search_tasks("agent"); });
  REQUIRE(results.size() == 1);
  CHECK(results[0]["title"] == "Refactor agent system");
}

TEST_CASE("task_tools: search by description")
{
  temp_tasks_dir tmp;
  mistral::tools::task_tools tasks(tmp.path.string());

  run_async_task_op([&]() {
    return tasks.create_task("Task", "Contains keyword like socket", "", {});
  });
  run_async_task_op([&]() {
    return tasks.create_task("Other", "No match here", "", {});
  });

  auto results = run_async_task_op([&]() { return tasks.search_tasks("socket"); });
  REQUIRE(results.size() == 1);
  CHECK(results[0]["matched_in"] == "description");
}

TEST_CASE("task_tools: search is case insensitive")
{
  temp_tasks_dir tmp;
  mistral::tools::task_tools tasks(tmp.path.string());

  run_async_task_op([&]() { return tasks.create_task("CMAKE Build", "", "", {}); });

  auto results = run_async_task_op([&]() { return tasks.search_tasks("cmake"); });
  REQUIRE(results.size() == 1);
}

TEST_CASE("task_tools: search with empty query fails")
{
  temp_tasks_dir tmp;
  mistral::tools::task_tools tasks(tmp.path.string());

  auto r = run_async_task_op([&]() { return tasks.search_tasks(""); });
  CHECK(r.contains("error"));
}

// ---- Persistence ----

TEST_CASE("task_tools: tasks persist across instances")
{
  temp_tasks_dir tmp;

  {
    mistral::tools::task_tools tasks1(tmp.path.string());
    auto r = run_async_task_op([&]() {
      return tasks1.create_task("Persistent Task", "Survives restart", "high", {"important"});
    });
    CHECK(r["id"] == "task-001");
  }

  {
    mistral::tools::task_tools tasks2(tmp.path.string());
    auto listing = run_async_task_op([&]() { return tasks2.list_tasks("", "", ""); });
    REQUIRE(listing.size() == 1);
    CHECK(listing[0]["title"] == "Persistent Task");

    auto task = run_async_task_op([&]() { return tasks2.get_task("task-001"); });
    CHECK(task["description"] == "Survives restart");
    CHECK(task["priority"] == "high");
  }
}