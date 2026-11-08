#ifndef MISTRAL_TUI_BACKEND_H
#define MISTRAL_TUI_BACKEND_H

#include "agent/agent_system.h"
#include "agent/agent_system_stats_task.h"
#include "agent/agent_system_task.h"
#include "agent/interactive_agent_task.h"
#include "agent/prompt_message.h"
#include "api/types.h"
#include "common/signal_watcher_task.h"
#include "common/wakeup_pipe.h"
#include "mcp/mcp_auto_connect_task.h"
#include "mcp/mcp_manager.h"
#include "slash_command/slash_command_executor_task.h"
#include "tui/tui_approval_state.h"
#include "tui/tui_bridge_tasks.h"
#include "tui/tui_render_sink.h"
#include "tui/tui_wakeup_queue.h"

#include <dripcore/channel.h>
#include <dripcore/scheduler.h>

#include <atomic>
#include <functional>
#include <memory>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace mistral::tui
{
  // -------------------------------------------------------------------------
  // tui_config: configuration shared between tui_main.cpp, tui_app, and
  // tui_backend. Contains agent identification and the dripcore channels
  // used for inter-task communication. Transport (endpoints, API keys,
  // coordinators) is owned by agent_system.
  // -------------------------------------------------------------------------
  struct tui_config
  {
    std::string agent_id;
    std::string theme_name;
    std::string resolved_model;
    // Static completion catalog (command specs + command names), built once
    // from the registry before the UI starts.
    mistral::slash_command::command_catalog command_catalog;
    std::shared_ptr<dripcore::channel<mistral::agent::prompt_message>> prompt_ch;
    std::shared_ptr<dripcore::channel<std::string>> input_ch;
    std::shared_ptr<dripcore::channel<std::string>> agent_response_ch;
    std::shared_ptr<dripcore::channel<mistral::slash_command::slash_command_request>> cmd_request_ch;
    std::shared_ptr<dripcore::channel<std::string>> slash_cmd_response_ch;
    bool resume = false;
  };

  // -------------------------------------------------------------------------
  // Signal handling for SIGINT -> dripcore event loop notification.
  // The write fd is stored in a global because signal handlers cannot access
  // class instances.
  // -------------------------------------------------------------------------
  inline int g_sigint_pipe_write_fd = -1;

  inline void sigint_handler(int)
  {
    if (g_sigint_pipe_write_fd >= 0)
    {
      // Nothing can be done from a signal handler if this fails.
      char buf = '1';
      [[maybe_unused]] ssize_t ret = ::write(g_sigint_pipe_write_fd, &buf, 1);
    }
  }

  // -------------------------------------------------------------------------
  // tui_backend: manages the dripcore background thread and all backend
  // tasks.
  //
  // Responsibilities:
  //   - Create and manage self-pipes for cross-thread wakeup (SIGINT, input,
  //     slash commands)
  //   - Install and restore the SIGINT handler
  //   - Launch the dripcore scheduler with all agent, HTTP, and bridge tasks
  //   - Provide thread-safe send_input() and send_slash_command() for the UI
  //     thread to push messages into the dripcore event loop
  //   - Own the agent_ready atomic shared between the dripcore thread
  //     (ui_bridge_task<prompt_message>) and the UI thread (timers)
  //   - Coordinate clean shutdown (signal dripcore, join thread, close pipes)
  //
  // The UI callbacks (post, append_content, append_fragment, flush) allow
  // the backend to route output back to the UI thread via App::post().
  // -------------------------------------------------------------------------
  class tui_backend
  {
  public:
    // Applied on the UI thread when the scheduler pushes a status snapshot.
    using ui_status_fn = std::function<void(const mistral::agent::ui_status&)>;

    struct ui_callbacks
    {
      post_fn post;
      append_fn append_content;
      append_fn append_fragment;
      flush_fn flush;
      completed_fn completed;
      approval_state_fn on_approval_state;
      ui_status_fn on_ui_status;
    };

    tui_backend(tui_config cfg, ui_callbacks callbacks) : cfg_(std::move(cfg)), callbacks_(std::move(callbacks))
    {
      g_sigint_pipe_write_fd = sigint_pipe_.write_fd();
      mistral::agent::agent_system::instance().set_shutdown_pipe_fd(sigint_pipe_.write_fd());

      struct sigaction sa{};
      sa.sa_handler = sigint_handler;
      sigemptyset(&sa.sa_mask);
      sa.sa_flags = 0;
      sigaction(SIGINT, &sa, nullptr);
    }

    ~tui_backend()
    {
      if (dripcore_thread_.joinable())
      {
        shutdown();
      }
    }

    tui_backend(const tui_backend&) = delete;
    tui_backend& operator=(const tui_backend&) = delete;

    // Launch the dripcore scheduler on a background thread. Spawns all
    // backend tasks (agent, HTTP coordinator, bridge tasks, signal
    // watcher).
    void start()
    {
      dripcore_thread_ = std::thread([this]() {
        dripcore::scheduler sched;
        auto& agent_system = mistral::agent::agent_system::instance();

        sched.spawn<mistral::agent::agent_system_task>(cfg_.agent_response_ch);
        sched.spawn<mistral::agent::agent_system_stats_task>(agent_system.stats_ch());
        agent_system.start_transport(sched, cfg_.agent_id);
        sched.spawn<mistral::tui::pipe_bridge_task<std::string>>(std::ref(input_q_), cfg_.input_ch);
        sched.spawn<mistral::slash_command::slash_command_executor_task>(
          cfg_.cmd_request_ch, cfg_.slash_cmd_response_ch, agent_system.slash_commands()
        );
        sched.spawn<mistral::tui::ui_bridge_task<std::string>>(
          cfg_.slash_cmd_response_ch,
          callbacks_.post,
          [append = callbacks_.append_content](std::string text) {
            append("\n" + text + "\n", mistral::tui::content_type::system);
          },
          agent_system.shutdown_signal()
        );
        sched.spawn<mistral::tui::pipe_bridge_task<mistral::slash_command::slash_command_request>>(
          std::ref(slash_cmd_q_), cfg_.cmd_request_ch
        );
        sched.spawn<mistral::tui::ui_bridge_task<mistral::agent::prompt_message>>(
          cfg_.prompt_ch,
          callbacks_.post,
          [this](mistral::agent::prompt_message msg) { apply_prompt_message(std::move(msg)); },
          agent_system.shutdown_signal()
        );
        sched.spawn<mistral::tui::ui_bridge_task<mistral::agent::ui_status>>(
          agent_system.ui_status_ch(), callbacks_.post, callbacks_.on_ui_status, agent_system.shutdown_signal()
        );

        auto sink = std::make_shared<mistral::tui::tui_render_sink>(
          callbacks_.post, callbacks_.append_fragment, callbacks_.flush, callbacks_.completed
        );
        sched.spawn<mistral::agent::interactive_agent_task>(
          cfg_.agent_id, cfg_.input_ch, cfg_.agent_response_ch, cfg_.prompt_ch, sink, cfg_.resume
        );

        sched.spawn<mistral::common::signal_watcher_task>(sigint_pipe_.read_fd());

        // Spawn MCP auto-connect task (connects servers with
        // auto_connect=true). Runs on the dripcore thread; it reports the
        // outcome to the content area through the sink and pushes a
        // ui_status snapshot for the footer.
        sched.spawn<mistral::mcp::mcp_auto_connect_task>(sink);

        sched.run();
      });
    }

    // Signal the dripcore thread to shut down, join it, and close all
    // pipes.
    void shutdown()
    {
      if (!dripcore_thread_.joinable())
        return;

      sigint_pipe_.notify('1');

      signal(SIGINT, SIG_DFL);
      g_sigint_pipe_write_fd = -1;

      input_q_.close_write();
      slash_cmd_q_.close_write();
      sigint_pipe_.close_write();

      dripcore_thread_.join();

      // Close MCP clients (kill stdio child processes) now that the
      // dripcore thread has finished — no coroutines are using them.
      // This must happen before process exit so child processes are
      // reaped in a controlled manner, not from static destructors.
      mistral::mcp::mcp_manager::instance().shutdown();

      input_q_.close_read();
      slash_cmd_q_.close_read();
      sigint_pipe_.close_read();
    }

    // Push a user message or approval response to the agent task.
    void send_input(std::string msg)
    {
      input_q_.push(std::move(msg));
    }

    // Push a slash command request to the slash command executor.
    void send_slash_command(mistral::slash_command::slash_command_request req)
    {
      slash_cmd_q_.push(std::move(req));
    }

    // Write fd for the SIGINT pipe — used by the UI's ESC key handler.
    int sigint_write_fd() const
    {
      return sigint_pipe_.write_fd();
    }

    // Atomics shared between dripcore thread and UI thread.
    std::atomic<bool>& agent_ready()
    {
      return agent_ready_;
    }

  private:
    // Apply a prompt message on the UI thread.
    void apply_prompt_message(mistral::agent::prompt_message msg)
    {
      switch (msg.type)
      {
      case mistral::agent::prompt_message::kind::ready:
        agent_ready_.store(true, std::memory_order_release);
        break;
      case mistral::agent::prompt_message::kind::approval:
        callbacks_.append_content(msg.text, mistral::tui::content_type::tool_approval);
        callbacks_.on_approval_state(approval_state::single_key);
        break;
      case mistral::agent::prompt_message::kind::ask:
        callbacks_.append_content(msg.text, mistral::tui::content_type::tool_approval);
        callbacks_.on_approval_state(approval_state::ask);
        break;
      case mistral::agent::prompt_message::kind::text:
        callbacks_.append_content(msg.text, mistral::tui::content_type::system);
        break;
      }
    }

    tui_config cfg_;
    ui_callbacks callbacks_;

    wakeup_queue<std::string> input_q_;
    wakeup_queue<mistral::slash_command::slash_command_request> slash_cmd_q_;

    std::atomic<bool> agent_ready_{true};

    mistral::common::wakeup_pipe sigint_pipe_;

    std::thread dripcore_thread_;
  };

} // namespace mistral::tui

#endif // MISTRAL_TUI_BACKEND_H
