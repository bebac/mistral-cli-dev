#ifndef MISTRAL_TUI_BRIDGE_TASKS_H
#define MISTRAL_TUI_BRIDGE_TASKS_H

#include "tui/thread_safe_queue.h"
#include "tui/tui_render_sink.h"
#include "slash_command/slash_command.h"
#include "trace/trace.h"

#include <dripcore/task.h>
#include <dripcore/channel.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <unistd.h>

namespace mistral::tui
{
  // Shared callback typedefs used by bridge tasks that post content
  // back to the TUI thread.
  using post_fn   = std::function<void(std::function<void()>)>;
  using append_fn = std::function<void(const std::string&,
                                       mistral::agent::content_type)>;

  // ---------------------------------------------------------------------------
  // prompt_bridge_task: runs in dripcore context, bridges prompt_ch_ to the
  // TUI thread. Reads prompt messages from the agent task and routes them:
  //   - "mistral" → sets agent_ready flag (agent is ready for input)
  //   - "[APPROVAL_PROMPT]..." → appends to content area via App::post,
  //     sets approval mode to single_key
  //   - Other (e.g. "Enter message for AI...") → appends to content area
  //     via App::post, sets approval mode to text_input
  //
  // This is necessary because dripcore channels can only be consumed from
  // within a dripcore task context.
  // ---------------------------------------------------------------------------
  class prompt_bridge_task : public dripcore::task
  {
  public:
    prompt_bridge_task(
      std::shared_ptr<dripcore::channel<std::string>> prompt_ch,
      std::atomic<bool>& agent_ready,
      std::atomic<int>& approval_state,
      post_fn post,
      append_fn append_content
    )
      : prompt_ch_(std::move(prompt_ch))
      , agent_ready_(agent_ready)
      , approval_state_(approval_state)
      , post_(std::move(post))
      , append_content_(std::move(append_content))
    {
    }

    dripcore::awaitable<> body() override
    {
      while (true)
      {
        auto msg = co_await prompt_ch_->recv(this);
        if (!msg) break;  // Channel closed.

        mistral::trace::logger::instance().trace(
          mistral::trace::category::approval,
          "prompt_bridge received: {}", msg->substr(0, 80)
        );

        if (msg->find("[APPROVAL_PROMPT]") == 0)
        {
          std::string prompt_text =
            msg->substr(std::string("[APPROVAL_PROMPT]").length());
          approval_state_.store(1, std::memory_order_release);
          post_([fn = append_content_, text = std::move(prompt_text)]() {
            fn(text, mistral::agent::content_type::tool_approval);
          });
        }
        else if (*msg == "mistral")
        {
          agent_ready_.store(true, std::memory_order_release);
        }
        else
        {
          // Other prompts (e.g. "Enter message for AI...") — append inline.
          // These are text-input prompts from the reject-with-message flow.
          // The agent task already sent the [APPROVAL_PROMPT] first, which
          // set approval_state to 1 (single_key). The user pressed 'r',
          // which set approval_state to 2 (text_input) and sent 'r' back.
          // Now the agent sends the text prompt. We just append it inline.
          post_([fn = append_content_, text = *msg]() {
            fn(text, mistral::agent::content_type::system);
          });
        }
      }

      mistral::trace::logger::instance().trace(
        mistral::trace::category::approval,
        "prompt_bridge channel closed, exiting"
      );
      co_return;
    }

  private:
    std::shared_ptr<dripcore::channel<std::string>> prompt_ch_;
    std::atomic<bool>& agent_ready_;
    std::atomic<int>& approval_state_;
    post_fn post_;
    append_fn append_content_;
  };

  // ---------------------------------------------------------------------------
  // pipe_bridge_task: generic dripcore task that bridges a thread-safe
  // queue (fed from the UI thread) to a dripcore channel (consumed in
  // the agent task). A self-pipe provides the cross-thread wakeup:
  //
  //   UI thread:  queue.push(item);  ::write(pipe_write_fd, "1", 1);
  //   dripcore:   co_await readable(pipe_read_fd);
  //               ::read(pipe_read_fd, ...);  // drain wakeup byte
  //               items = queue.drain();
  //               for (auto& item : items) target_ch->try_send(std::move(item));
  //
  // On pipe EOF (write end closed during shutdown), the target channel is
  // closed and the optional on_eof callback is invoked (e.g. to close
  // additional channels). The task then exits.
  //
  // This replaces the former input_bridge_task and slash_command_bridge_task
  // which were structurally identical except for the element type and the
  // set of channels to close on EOF.
  // ---------------------------------------------------------------------------
  template<typename T>
  class pipe_bridge_task : public dripcore::task
  {
  public:
    using cleanup_fn = std::function<void()>;

    pipe_bridge_task(
      int pipe_read_fd,
      std::shared_ptr<dripcore::channel<T>> target_ch,
      thread_safe_queue<T>& queue,
      cleanup_fn on_eof = nullptr
    )
      : pipe_read_fd_(pipe_read_fd)
      , target_ch_(std::move(target_ch))
      , queue_(queue)
      , on_eof_(std::move(on_eof))
    {
    }

    dripcore::awaitable<> body() override
    {
      while (true)
      {
        co_await readable(pipe_read_fd_);

        char buf[16];
        ssize_t n = ::read(pipe_read_fd_, buf, sizeof(buf));

        if (n <= 0) {
          // EOF — pipe write end closed, shutdown.
          target_ch_->close();
          if (on_eof_) on_eof_();
          co_return;
        }

        // Drain all pending items and send to target channel.
        auto items = queue_.drain();
        for (auto& item : items) {
          target_ch_->try_send(std::move(item));
        }
      }

      co_return;
    }

  private:
    int pipe_read_fd_;
    std::shared_ptr<dripcore::channel<T>> target_ch_;
    thread_safe_queue<T>& queue_;
    cleanup_fn on_eof_;
  };

  // ---------------------------------------------------------------------------
  // slash_command_response_receiver: sub-task spawned alongside
  // pipe_bridge_task<slash_command_request>. Continuously receives
  // responses from the slash command executor and posts them to the
  // TUI content area.
  // ---------------------------------------------------------------------------
  class slash_command_response_receiver : public dripcore::task
  {
  public:
    slash_command_response_receiver(
      std::shared_ptr<dripcore::channel<std::string>> slash_cmd_response_ch,
      post_fn post,
      append_fn append_content
    )
      : slash_cmd_response_ch_(std::move(slash_cmd_response_ch))
      , post_(std::move(post))
      , append_content_(std::move(append_content))
    {
    }

    dripcore::awaitable<> body() override
    {
      while (true)
      {
        auto response = co_await slash_cmd_response_ch_->recv(this);
        if (!response) break;  // Channel closed.

        post_([fn = append_content_, text = std::move(*response)]() {
          fn("\n" + text + "\n", mistral::agent::content_type::system);
        });
      }

      co_return;
    }

  private:
    std::shared_ptr<dripcore::channel<std::string>> slash_cmd_response_ch_;
    post_fn post_;
    append_fn append_content_;
  };

} // namespace mistral::tui

#endif // MISTRAL_TUI_BRIDGE_TASKS_H