#ifndef MISTRAL_TUI_BACKEND_H
#define MISTRAL_TUI_BACKEND_H

#include "agent/agent_system.h"
#include "agent/agent_system_stats_task.h"
#include "agent/agent_system_task.h"
#include "agent/interactive_agent_task.h"
#include "api/types.h"
#include "common/signal_watcher_task.h"
#include "common/wakeup_pipe.h"
#include "mcp/mcp_auto_connect_task.h"
#include "mcp/mcp_manager.h"
#include "slash_command/slash_command_executor_task.h"
#include "tui/tui_approval_state.h"
#include "tui/tui_bridge_tasks.h"
#include "tui/tui_render_sink.h"
#include "tui/tui_thread_safe_queue.h"

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
    std::shared_ptr<dripcore::channel<std::string>> prompt_ch;
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
      char buf = '1';
      ssize_t ret = ::write(g_sigint_pipe_write_fd, &buf, 1);
      (void)ret;
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
  //     (prompt_bridge_task) and the UI thread (timers)
  //   - Coordinate clean shutdown (signal dripcore, join thread, close pipes)
  //
  // The UI callbacks (post, append_content, append_fragment, flush) allow
  // the backend to route output back to the UI thread via App::post().
  // -------------------------------------------------------------------------
  class tui_backend
  {
  public:
    struct ui_callbacks
    {
      post_fn post;
      append_fn append_content;
      append_fn append_fragment;
      flush_fn flush;
      completed_fn completed;
      approval_state_fn on_approval_state;
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
        agent_system.start_transport(sched);
        sched.spawn<mistral::tui::pipe_bridge_task<std::string>>(input_pipe_.read_fd(), cfg_.input_ch, std::ref(inq_));
        sched.spawn<mistral::slash_command::slash_command_executor_task>(
          cfg_.cmd_request_ch, cfg_.slash_cmd_response_ch, agent_system.slash_commands()
        );
        sched.spawn<mistral::tui::slash_command_response_receiver>(
          cfg_.slash_cmd_response_ch, callbacks_.post, callbacks_.append_content
        );
        sched.spawn<mistral::tui::pipe_bridge_task<mistral::slash_command::slash_command_request>>(
          slash_cmd_pipe_.read_fd(),
          cfg_.cmd_request_ch,
          std::ref(slash_cmd_queue_),
          [ch = cfg_.slash_cmd_response_ch]() { ch->close(); }
        );
        sched.spawn<mistral::tui::prompt_bridge_task>(
          cfg_.prompt_ch,
          std::ref(agent_ready_),
          callbacks_.post,
          callbacks_.append_content,
          callbacks_.on_approval_state
        );

        auto sink = std::make_shared<mistral::tui::tui_render_sink>(
          callbacks_.post, callbacks_.append_fragment, callbacks_.flush, callbacks_.completed
        );
        sched.spawn<mistral::agent::interactive_agent_task>(
          cfg_.agent_id, cfg_.input_ch, cfg_.agent_response_ch, cfg_.prompt_ch, sink, cfg_.resume
        );

        sched.spawn<mistral::common::signal_watcher_task>(sigint_pipe_.read_fd());

        // Spawn MCP auto-connect task (connects servers with
        // auto_connect=true). Runs on the dripcore thread; status
        // is surfaced via the TUI footer (polled in tui_app timer).
        sched.spawn<mistral::mcp::mcp_auto_connect_task>();

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

      input_pipe_.close_write();
      slash_cmd_pipe_.close_write();
      sigint_pipe_.close_write();

      dripcore_thread_.join();

      // Close MCP clients (kill stdio child processes) now that the
      // dripcore thread has finished — no coroutines are using them.
      // This must happen before process exit so child processes are
      // reaped in a controlled manner, not from static destructors.
      mistral::mcp::mcp_manager::instance().shutdown();

      input_pipe_.close_read();
      slash_cmd_pipe_.close_read();
      sigint_pipe_.close_read();
    }

    // Push a user message or approval response to the agent task.
    void send_input(std::string msg)
    {
      inq_.push(std::move(msg));
      input_pipe_.notify();
    }

    // Push a slash command request to the slash command executor.
    void send_slash_command(mistral::slash_command::slash_command_request req)
    {
      slash_cmd_queue_.push(std::move(req));
      slash_cmd_pipe_.notify();
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
    tui_config cfg_;
    ui_callbacks callbacks_;

    thread_safe_queue<std::string> inq_;
    thread_safe_queue<mistral::slash_command::slash_command_request> slash_cmd_queue_;

    std::atomic<bool> agent_ready_{true};

    mistral::common::wakeup_pipe sigint_pipe_;
    mistral::common::wakeup_pipe input_pipe_;
    mistral::common::wakeup_pipe slash_cmd_pipe_;

    std::thread dripcore_thread_;
  };

} // namespace mistral::tui

#endif // MISTRAL_TUI_BACKEND_H
