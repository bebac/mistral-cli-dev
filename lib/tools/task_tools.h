#ifndef MISTRAL_TASK_TOOLS_H
#define MISTRAL_TASK_TOOLS_H

#include <dripcore/awaitable.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace mistral::tools
{
  using json = nlohmann::ordered_json;

  // Structured task management with cross-session persistence.
  // Tasks are stored as a JSON file in the notes directory.
  //
  // Each task has:
  //   id          - auto-generated unique identifier (e.g. "task-001")
  //   title       - short summary
  //   description - detailed explanation (optional)
  //   status      - one of: todo, in-progress, done, blocked, cancelled
  //   priority    - one of: low, medium, high, critical
  //   tags        - list of string tags for categorisation
  //   created_at  - ISO 8601 timestamp
  //   updated_at  - ISO 8601 timestamp
  class task_tools
  {
  public:
    explicit task_tools(std::string notes_dir);

    // Create a new task. Returns the created task object with generated ID.
    dripcore::awaitable<json> create_task(
      std::string title,
      std::string description,
      std::string priority,
      std::vector<std::string> tags
    );

    // List tasks with optional filters. Empty filter values mean "no filter".
    // Returns an array of task summaries (id, title, status, priority, tags).
    dripcore::awaitable<json> list_tasks(std::string status, std::string tag, std::string priority);

    // Get full details of a task by ID.
    dripcore::awaitable<json> get_task(std::string task_id);

    // Update a task. Empty string fields are not changed.
    // Returns the updated task object.
    dripcore::awaitable<json> update_task(
      std::string task_id,
      std::string status,
      std::string priority,
      std::string title,
      std::string description
    );

    // Search tasks by text query across title and description (case-insensitive).
    // Returns an array of matching task summaries.
    dripcore::awaitable<json> search_tasks(std::string query);

  private:
    // Load all tasks from the JSON store. Returns empty array if file missing.
    json load_tasks() const;

    // Save tasks to the JSON store. Creates parent directories if needed.
    void save_tasks(const json& tasks) const;

    // Generate the next sequential task ID (e.g. "task-001", "task-002").
    std::string generate_id(const json& tasks) const;

    // Return the current UTC time as an ISO 8601 string.
    std::string current_timestamp() const;

    // Build a summary JSON object from a full task object.
    json task_summary(const json& task) const;

    std::string notes_dir_;
    std::string tasks_file_;
  };

} // namespace mistral::tools

#endif // MISTRAL_TASK_TOOLS_H
