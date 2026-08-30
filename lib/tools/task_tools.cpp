#include "tools/task_tools.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace mistral::tools
{
  // ---- Valid enum values ----

  static const std::vector<std::string> VALID_STATUSES = {
    "todo", "in-progress", "done", "blocked", "cancelled"
  };

  static const std::vector<std::string> VALID_PRIORITIES = {
    "low", "medium", "high", "critical"
  };

  static bool is_valid_status(const std::string& s)
  {
    return std::find(VALID_STATUSES.begin(), VALID_STATUSES.end(), s)
      != VALID_STATUSES.end();
  }

  static bool is_valid_priority(const std::string& p)
  {
    return std::find(VALID_PRIORITIES.begin(), VALID_PRIORITIES.end(), p)
      != VALID_PRIORITIES.end();
  }

  // ---- task_tools ----

  task_tools::task_tools(std::string notes_dir)
    : notes_dir_(std::move(notes_dir))
    , tasks_file_((fs::path(notes_dir_) / "_tasks" / "tasks.json").string())
  {
    // Ensure the _tasks directory exists.
    fs::create_directories(fs::path(notes_dir_) / "_tasks");
  }

  std::string task_tools::current_timestamp() const
  {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream ss;
    ss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
  }

  std::string task_tools::generate_id(const json& tasks) const
  {
    int max_num = 0;
    for (const auto& task : tasks) {
      if (task.contains("id")) {
        std::string id = task["id"].get<std::string>();
        // Parse "task-NNN" format.
        if (id.starts_with("task-")) {
          try {
            int num = std::stoi(id.substr(5));
            if (num > max_num) max_num = num;
          } catch (...) {}
        }
      }
    }
    return std::format("task-{:03d}", max_num + 1);
  }

  json task_tools::load_tasks() const
  {
    fs::path file_path(tasks_file_);
    if (!fs::exists(file_path)) {
      return json::array();
    }

    std::ifstream f(file_path);
    if (!f) {
      return json::array();
    }

    try {
      json data = json::parse(f);
      if (data.is_array()) {
        return data;
      }
    } catch (...) {}

    return json::array();
  }

  void task_tools::save_tasks(const json& tasks) const
  {
    fs::path file_path(tasks_file_);
    fs::create_directories(file_path.parent_path());

    std::ofstream f(file_path);
    if (!f) {
      std::cerr << "warning: could not write tasks file: " << tasks_file_
                << std::endl;
      return;
    }

    f << tasks.dump(2);
    f.close();
  }

  json task_tools::task_summary(const json& task) const
  {
    json summary;
    summary["id"] = task["id"];
    summary["title"] = task["title"];
    summary["status"] = task["status"];
    summary["priority"] = task["priority"];
    if (task.contains("tags") && !task["tags"].empty()) {
      summary["tags"] = task["tags"];
    }
    return summary;
  }

  // ---- Tool methods ----

  dripcore::awaitable<json> task_tools::create_task(
    std::string title,
    std::string description,
    std::string priority,
    std::vector<std::string> tags
  )
  {
    if (title.empty()) {
      co_return {{"error", "title is required"}};
    }

    // Validate priority.
    if (!priority.empty() && !is_valid_priority(priority)) {
      co_return {{"error", std::format(
        "invalid priority '{}'. Valid values: low, medium, high, critical",
        priority)}};
    }
    if (priority.empty()) {
      priority = "medium";
    }

    json tasks = load_tasks();
    std::string id = generate_id(tasks);
    std::string ts = current_timestamp();

    json task;
    task["id"] = id;
    task["title"] = title;
    task["description"] = description;
    task["status"] = "todo";
    task["priority"] = priority;
    if (!tags.empty()) {
      task["tags"] = tags;
    } else {
      task["tags"] = json::array();
    }
    task["created_at"] = ts;
    task["updated_at"] = ts;

    tasks.push_back(task);
    save_tasks(tasks);

    co_return task;
  }

  dripcore::awaitable<json> task_tools::list_tasks(
    std::string status,
    std::string tag,
    std::string priority
  )
  {
    // Validate filters if provided.
    if (!status.empty() && !is_valid_status(status)) {
      co_return {{"error", std::format(
        "invalid status '{}'. Valid values: todo, in-progress, done, blocked, cancelled",
        status)}};
    }
    if (!priority.empty() && !is_valid_priority(priority)) {
      co_return {{"error", std::format(
        "invalid priority '{}'. Valid values: low, medium, high, critical",
        priority)}};
    }

    // Lowercase filters for case-insensitive matching.
    std::string status_l = status, tag_l = tag, priority_l = priority;
    std::transform(status_l.begin(), status_l.end(), status_l.begin(), ::tolower);
    std::transform(tag_l.begin(), tag_l.end(), tag_l.begin(), ::tolower);
    std::transform(priority_l.begin(), priority_l.end(), priority_l.begin(), ::tolower);

    json tasks = load_tasks();
    json results = json::array();

    for (const auto& task : tasks) {
      // Filter by status.
      if (!status_l.empty()) {
        std::string s = task.value("status", "");
        std::string s_l = s;
        std::transform(s_l.begin(), s_l.end(), s_l.begin(), ::tolower);
        if (s_l != status_l) continue;
      }

      // Filter by priority.
      if (!priority_l.empty()) {
        std::string p = task.value("priority", "");
        std::string p_l = p;
        std::transform(p_l.begin(), p_l.end(), p_l.begin(), ::tolower);
        if (p_l != priority_l) continue;
      }

      // Filter by tag.
      if (!tag_l.empty() && task.contains("tags")) {
        bool found = false;
        for (const auto& t : task["tags"]) {
          std::string t_l = t.get<std::string>();
          std::transform(t_l.begin(), t_l.end(), t_l.begin(), ::tolower);
          if (t_l == tag_l) { found = true; break; }
        }
        if (!found) continue;
      }

      results.push_back(task_summary(task));
    }

    co_return results;
  }

  dripcore::awaitable<json> task_tools::get_task(std::string task_id)
  {
    json tasks = load_tasks();

    for (const auto& task : tasks) {
      if (task.value("id", "") == task_id) {
        co_return task;
      }
    }

    co_return {{"error", std::format("task not found: {}", task_id)}};
  }

  dripcore::awaitable<json> task_tools::update_task(
    std::string task_id,
    std::string status,
    std::string priority,
    std::string title,
    std::string description
  )
  {
    // Validate status if provided.
    if (!status.empty() && !is_valid_status(status)) {
      co_return {{"error", std::format(
        "invalid status '{}'. Valid values: todo, in-progress, done, blocked, cancelled",
        status)}};
    }
    // Validate priority if provided.
    if (!priority.empty() && !is_valid_priority(priority)) {
      co_return {{"error", std::format(
        "invalid priority '{}'. Valid values: low, medium, high, critical",
        priority)}};
    }

    json tasks = load_tasks();

    for (auto& task : tasks) {
      if (task.value("id", "") == task_id) {
        if (!status.empty()) task["status"] = status;
        if (!priority.empty()) task["priority"] = priority;
        if (!title.empty()) task["title"] = title;
        if (!description.empty()) task["description"] = description;
        task["updated_at"] = current_timestamp();

        save_tasks(tasks);
        co_return task;
      }
    }

    co_return {{"error", std::format("task not found: {}", task_id)}};
  }

  dripcore::awaitable<json> task_tools::search_tasks(std::string query)
  {
    if (query.empty()) {
      co_return {{"error", "query is required"}};
    }

    std::string query_l = query;
    std::transform(query_l.begin(), query_l.end(), query_l.begin(), ::tolower);

    json tasks = load_tasks();
    json results = json::array();

    for (const auto& task : tasks) {
      std::string title = task.value("title", "");
      std::string desc = task.value("description", "");

      std::string title_l = title;
      std::string desc_l = desc;
      std::transform(title_l.begin(), title_l.end(), title_l.begin(), ::tolower);
      std::transform(desc_l.begin(), desc_l.end(), desc_l.begin(), ::tolower);

      if (title_l.find(query_l) != std::string::npos ||
          desc_l.find(query_l) != std::string::npos) {
        json match = task_summary(task);
        // Include matching snippet for context.
        if (desc_l.find(query_l) != std::string::npos) {
          match["matched_in"] = "description";
        } else {
          match["matched_in"] = "title";
        }
        results.push_back(std::move(match));
      }
    }

    co_return results;
  }

} // namespace mistral::tools