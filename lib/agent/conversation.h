#ifndef MISTRAL_CONVERSATION_H
#define MISTRAL_CONVERSATION_H

#include "api/mistral_types.h"

#include <string>
#include <vector>

namespace mistral::agent
{
  class conversation
  {
  public:
    explicit conversation(size_t max_history = 0)
      : max_history_(max_history) {}

    void add_user_message(const std::string& content)
    {
      messages_.push_back({.role = "user", .content = content});
    }

    void add_assistant_message(const std::string& content,
                               std::optional<std::vector<api::tool_call>> tool_calls = std::nullopt)
    {
      messages_.push_back({
        .role = "assistant",
        .content = content,
        .tool_calls = std::move(tool_calls)
      });
    }

    void add_tool_result(const std::string& name,
                         const std::string& tool_call_id,
                         const std::string& content)
    {
      messages_.push_back({
        .role = "tool",
        .content = content,
        .name = name,
        .tool_call_id = tool_call_id
      });
    }

    void set_system_message(const std::string& content)
    {
      // System message is always at the front.
      if (!messages_.empty() && messages_.front().role == "system") {
        messages_.front().content = content;
      } else {
        messages_.insert(messages_.begin(), {.role = "system", .content = content});
      }
    }

    const std::vector<api::message>& messages() const { return messages_; }

    void ensure_window_size()
    {
      if (max_history_ == 0 || messages_.size() <= max_history_) return;

      // Keep system message (if any) + max_history_ conversation messages.
      size_t system_offset = 0;
      if (!messages_.empty() && messages_.front().role == "system") {
        system_offset = 1;
      }

      // Total messages to keep: system message + max_history_ conversation messages
      size_t total_to_keep = system_offset + max_history_;

      // If we have more messages than we want to keep, trim from the oldest
      if (messages_.size() > total_to_keep) {
        // Calculate how many messages to remove from the beginning (after system message)
        size_t messages_to_remove = messages_.size() - total_to_keep;

        // Start trimming after the system message
        auto first_to_remove = messages_.begin() + system_offset;
        auto last_to_remove = first_to_remove + messages_to_remove;

        messages_.erase(first_to_remove, last_to_remove);
      }
    }

    void clear() { messages_.clear(); }
    size_t size() const { return messages_.size(); }

  private:
    std::vector<api::message> messages_;
    size_t max_history_;
  };

} // namespace mistral::agent

#endif // MISTRAL_CONVERSATION_H
