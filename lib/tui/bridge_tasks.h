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
            fn(text, mistral::agent::content_type::system);
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
  // input_bridge_task: runs in dripcore context, bridges TUI input to the
  // agent's dripcore::channel. This is necessary because dripcore channels
  // can only be used from within a dripcore task context (they call
  // executor::current() internally).
  //
  // Protocol:
  //   Pipe readable → drain input_queue, send each message to input_ch
  //   Pipe EOF (write end closed) → close input_ch, exit
  // ---------------------------------------------------------------------------
  class input_bridge_task : public dripcore::task
  {
  public:
    input_bridge_task(
      int pipe_read_fd,
      std::shared_ptr<dripcore::channel<std::string>> input_ch,
      thread_safe_queue<std::string>& queue
    )
      : pipe_read_fd_(pipe_read_fd)
      , input_ch_(std::move(input_ch))
      , queue_(queue)
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
          input_ch_->close();
          co_return;
        }

        // Drain all pending messages and send to agent.
        auto messages = queue_.drain();
        for (auto& msg : messages) {
          input_ch_->try_send(std::move(msg));
        }
      }

      co_return;
    }

  private:
    int pipe_read_fd_;
    std::shared_ptr<dripcore::channel<std::string>> input_ch_;
    thread_safe_queue<std::string>& queue_;
  };

  // ---------------------------------------------------------------------------
  // slash_command_response_receiver: sub-task spawned by
  // slash_command_bridge_task. Continuously receives responses from the
  // slash command executor and posts them to the TUI content area.
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

  // ---------------------------------------------------------------------------
  // slash_command_bridge_task: runs in dripcore context, bridges the TUI
  // slash command queue to the executor task. On pipe readable, drains the
  // queue and sends requests to cmd_request_ch.
  // ---------------------------------------------------------------------------
  class slash_command_bridge_task : public dripcore::task
  {
  public:
    slash_command_bridge_task(
      int pipe_read_fd,
      std::shared_ptr<dripcore::channel<mistral::slash_command::slash_command_request>> cmd_request_ch,
      std::shared_ptr<dripcore::channel<std::string>> slash_cmd_response_ch,
      thread_safe_queue<mistral::slash_command::slash_command_request>& queue
    )
      : pipe_read_fd_(pipe_read_fd)
      , cmd_request_ch_(std::move(cmd_request_ch))
      , slash_cmd_response_ch_(std::move(slash_cmd_response_ch))
      , queue_(queue)
    {
    }

    dripcore::awaitable<> body() override
    {
      // Main loop: wait for pipe wakeup, drain queue, send requests.
      while (true)
      {
        co_await readable(pipe_read_fd_);

        char buf[16];
        ssize_t n = ::read(pipe_read_fd_, buf, sizeof(buf));

        if (n <= 0) {
          // EOF — pipe write end closed, shutdown.
          cmd_request_ch_->close();
          slash_cmd_response_ch_->close();
          co_return;
        }

        // Drain all pending requests and send to executor.
        auto requests = queue_.drain();
        for (auto& req : requests) {
          cmd_request_ch_->try_send(std::move(req));
        }
      }

      co_return;
    }

  private:
    int pipe_read_fd_;
    std::shared_ptr<dripcore::channel<mistral::slash_command::slash_command_request>> cmd_request_ch_;
    std::shared_ptr<dripcore::channel<std::string>> slash_cmd_response_ch_;
    thread_safe_queue<mistral::slash_command::slash_command_request>& queue_;
  };

} // namespace mistral::tui

#endif // MISTRAL_TUI_BRIDGE_TASKS_H