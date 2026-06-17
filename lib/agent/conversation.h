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

    void add_assistant_message(const std::vector<api::content_chunk>& content,
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
        messages_.front().content = std::string(content);
      } else {
        api::message sys_msg;
        sys_msg.role = "system";
        sys_msg.content = std::string(content);
        messages_.insert(messages_.begin(), std::move(sys_msg));
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

      if (messages_[system_offset].role != "user")
      {
        // TODO. First message after the system message (if present) must
        // always be a user message. Not sure extaclty how to handle this.
        // It should probably be an assertion or an exception as it would
        // be a violation of the chat alteration and means that our
        // mistral-cli logic is incorrect.
        return;
      }

      while (messages_.size() > system_offset + max_history_)
      {
        auto first_to_remove = messages_.begin() + system_offset;

        // Find the next user message. We want to remove complete turns
        // (user + assistant [+ tool results]) to preserve alternation.
        auto next_user_message = std::find_if(first_to_remove+1, messages_.end(),
          [](const api::message& msg) {
            return msg.role == "user";
          }
        );

        if ( next_user_message == messages_.end() ) {
          // We cannot remove the active turn.
          return;
        }
        else {
          // Remove messages up to the next user message (exclusive).
          messages_.erase(first_to_remove, next_user_message);
        }
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
