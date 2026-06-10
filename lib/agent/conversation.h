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

      // Keep system message (if any) + the most recent messages.
      size_t start = 0;
      if (!messages_.empty() && messages_.front().role == "system") {
        start = 1;
      }

      size_t keep = max_history_;
      if (messages_.size() - start > keep) {
        messages_.erase(messages_.begin() + start,
                        messages_.end() - keep);
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
