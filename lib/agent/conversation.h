#ifndef MISTRAL_CONVERSATION_H
#define MISTRAL_CONVERSATION_H

#include "api/types.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mistral::agent
{
  class conversation
  {
  public:
    explicit conversation(size_t max_history = 0) : max_history_(max_history)
    {
    }

    void add_user_message(const std::string& content)
    {
      api::message_content mc;
      mc.type = "text";
      mc.content = content;
      messages_.push_back({.role = "user", .content = {std::move(mc)}});
    }

    void add_assistant_message(
      const std::string& content,
      std::optional<std::vector<api::tool_call>> tool_calls = std::nullopt
    )
    {
      api::message_content mc;
      mc.type = "text";
      mc.content = content;
      messages_.push_back({.role = "assistant", .content = {std::move(mc)}, .tool_calls = std::move(tool_calls)});
    }

    void add_assistant_message(
      const std::vector<api::message_content>& content,
      std::optional<std::vector<api::tool_call>> tool_calls = std::nullopt
    )
    {
      messages_.push_back({.role = "assistant", .content = content, .tool_calls = std::move(tool_calls)});
    }

    void add_tool_result(const std::string& name, const std::string& tool_call_id, const std::string& content)
    {
      api::message_content mc;
      mc.type = "text";
      mc.content = content;
      messages_.push_back({.role = "tool", .content = {std::move(mc)}, .name = name, .tool_call_id = tool_call_id});
    }

    void set_system_message(const std::string& content)
    {
      // System message is always at the front.
      if (!messages_.empty() && messages_.front().role == "system")
      {
        api::message_content mc;
        mc.type = "text";
        mc.content = content;
        messages_.front().content = {std::move(mc)};
      }
      else
      {
        api::message sys_msg;
        sys_msg.role = "system";
        api::message_content mc;
        mc.type = "text";
        mc.content = content;
        sys_msg.content = {std::move(mc)};
        messages_.insert(messages_.begin(), std::move(sys_msg));
      }
    }

    const std::vector<api::message>& messages() const
    {
      return messages_;
    }

    void ensure_window_size()
    {
      if (max_history_ == 0 || messages_.size() <= max_history_)
        return;

      // Keep system message (if any) + max_history_ conversation messages.
      size_t system_offset = 0;

      if (!messages_.empty() && messages_.front().role == "system")
      {
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
        auto next_user_message = std::find_if(first_to_remove + 1, messages_.end(), [](const api::message& msg) {
          return msg.role == "user";
        });

        if (next_user_message == messages_.end())
        {
          // We cannot remove the active turn.
          return;
        }
        else
        {
          // Remove messages up to the next user message (exclusive).
          messages_.erase(first_to_remove, next_user_message);
        }
      }
    }

    // Number of complete turns (user-initiated message groups) in the
    // history, excluding the system message. Tool results and assistant
    // messages belong to the turn that produced them.
    size_t turn_count() const
    {
      size_t system_offset = has_system_message() ? 1 : 0;
      size_t turns = 0;
      for (size_t i = system_offset; i < messages_.size(); ++i)
      {
        if (messages_[i].role == "user")
          ++turns;
      }
      return turns;
    }

    // Messages of the most recent turn: from the last user message to the
    // end of the history. Empty when there is no user message.
    std::vector<api::message> last_turn() const
    {
      for (size_t i = messages_.size(); i-- > 0;)
      {
        if (messages_[i].role == "user")
        {
          return std::vector<api::message>(messages_.begin() + i, messages_.end());
        }
      }
      return {};
    }

    // Messages that apply_compaction() would replace: everything after
    // the system message (if any) and before the recent tail (the last
    // keep_turns turns). Empty when there is nothing to compact.
    std::vector<api::message> messages_to_compact(size_t keep_turns) const
    {
      if (turn_count() <= keep_turns)
        return {};

      size_t system_offset = has_system_message() ? 1 : 0;
      size_t tail = compaction_tail_index(keep_turns);
      return std::vector<api::message>(messages_.begin() + system_offset, messages_.begin() + tail);
    }

    // Replace the messages returned by messages_to_compact() with a
    // synthetic summary turn (user + assistant). The system message and
    // the last keep_turns turns are preserved verbatim. No-op when
    // there is nothing to compact.
    void apply_compaction(const std::string& summary, size_t keep_turns)
    {
      if (turn_count() <= keep_turns)
        return;

      size_t system_offset = has_system_message() ? 1 : 0;
      size_t tail = compaction_tail_index(keep_turns);

      std::vector<api::message> compacted;
      compacted.reserve(system_offset + 2 + (messages_.size() - tail));
      compacted.insert(compacted.end(), messages_.begin(), messages_.begin() + system_offset);

      // The user message carries the summary so the stored history
      // stays self-contained. The assistant acknowledgement preserves
      // user/assistant alternation for the kept tail.
      api::message summary_msg;
      summary_msg.role = "user";
      summary_msg.content = {api::message_content{
        .type = "text",
        .content = std::format(
          "The earlier part of this conversation was compacted to free up "
          "context space. Summary of the previous conversation:\n\n{}",
          summary
        )
      }};
      compacted.push_back(std::move(summary_msg));

      api::message ack_msg;
      ack_msg.role = "assistant";
      ack_msg.content = {
        api::message_content{.type = "text", .content = "Understood. Continuing with the compacted context in mind."}
      };
      compacted.push_back(std::move(ack_msg));

      compacted.insert(compacted.end(), messages_.begin() + tail, messages_.end());
      messages_ = std::move(compacted);
    }

    void clear()
    {
      messages_.clear();
    }
    size_t size() const
    {
      return messages_.size();
    }

    void save(const std::string& base_dir, const std::string& session_id) const
    {
      std::filesystem::path chat_dir = std::filesystem::path(base_dir) / "chats";
      std::filesystem::create_directories(chat_dir);

      std::string chat_path = std::format("{}/chat_{}.json", chat_dir.string(), session_id);
      save_to_file(chat_path);

      // Create/update latest symlink
      std::filesystem::path latest_path = chat_dir / "chat.latest.json";
      std::filesystem::remove(latest_path);
      std::filesystem::create_symlink(std::filesystem::path(chat_path).filename(), latest_path);
    }

    // Snapshot of the full history before a compaction, so the original
    // conversation can be recovered. Does not touch the chat.latest.json
    // symlink.
    void save_snapshot(const std::string& base_dir, const std::string& session_id) const
    {
      std::filesystem::path chat_dir = std::filesystem::path(base_dir) / "chats";
      std::filesystem::create_directories(chat_dir);

      save_to_file(std::format("{}/chat_{}_precompact.json", chat_dir.string(), session_id));
    }

    // Load messages from a chat file written by save(). Replaces the
    // current history. Returns the number of messages loaded.
    // Throws std::runtime_error when the file is unreadable or malformed.
    size_t load(const std::string& chat_path)
    {
      std::ifstream in(chat_path);
      if (!in)
      {
        throw std::runtime_error("Failed to open chat file: " + chat_path);
      }

      api::json j;
      try
      {
        in >> j;
      }
      catch (const std::exception& e)
      {
        throw std::runtime_error(std::format("Failed to parse chat file {}: {}", chat_path, e.what()));
      }

      if (!j.is_array())
      {
        throw std::runtime_error(std::format("Malformed chat file {}: expected a JSON array of messages", chat_path));
      }

      std::vector<api::message> loaded;
      loaded.reserve(j.size());
      for (const auto& msg_json : j)
      {
        loaded.push_back(api::message_from_json(msg_json));
      }

      messages_ = std::move(loaded);
      return messages_.size();
    }

    // Extract the session id from a chat file name ("chat_<id>.json").
    // Returns nullopt when the name doesn't match the scheme.
    static std::optional<std::string> session_id_from_filename(const std::string& filename)
    {
      constexpr std::string_view prefix = "chat_";
      constexpr std::string_view suffix = ".json";
      if (filename.size() <= prefix.size() + suffix.size())
      {
        return std::nullopt;
      }
      if (filename.compare(0, prefix.size(), prefix) != 0)
      {
        return std::nullopt;
      }
      if (filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) != 0)
      {
        return std::nullopt;
      }
      return filename.substr(prefix.size(), filename.size() - prefix.size() - suffix.size());
    }

  private:
    bool has_system_message() const
    {
      return !messages_.empty() && messages_.front().role == "system";
    }

    // Index of the first message of the recent tail (the last keep_turns
    // turns). Returns messages_.size() when the whole conversation is in
    // the tail, or when the tail is empty (keep_turns == 0). Callers must
    // guard with turn_count() > keep_turns.
    size_t compaction_tail_index(size_t keep_turns) const
    {
      size_t system_offset = has_system_message() ? 1 : 0;
      size_t turns_to_skip = turn_count() - keep_turns;
      for (size_t i = system_offset; i < messages_.size(); ++i)
      {
        if (messages_[i].role == "user")
        {
          if (turns_to_skip == 0)
            return i;
          --turns_to_skip;
        }
      }
      return messages_.size();
    }

    void save_to_file(const std::string& chat_path) const
    {
      std::ofstream out(chat_path);
      if (out)
      {
        api::json j = api::json::array();
        for (const auto& msg : messages_)
        {
          j.push_back(api::message_to_json(msg, true)); // Include thinking for logging
        }
        out << j.dump(2);
      }
      else
      {
        throw std::runtime_error("Failed to open file: " + chat_path);
      }
    }

    std::vector<api::message> messages_;
    size_t max_history_;
  };

} // namespace mistral::agent

#endif // MISTRAL_CONVERSATION_H
