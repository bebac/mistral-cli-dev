#ifndef MISTRAL_TUI_APP_H
#define MISTRAL_TUI_APP_H

#include "tui/thread_safe_queue.h"
#include "tui/tui_render_sink.h"
#include "tui/tui_widgets.h"
#include "tui/bridge_tasks.h"
#include "agent/agent_system.h"
#include "agent/agent_system_task.h"
#include "agent/agent_system_stats_task.h"
#include "agent/interactive_agent_task.h"
#include "slash_command/slash_command_executor_task.h"
#include "common/signal_watcher_task.h"
#include "http/http_coordinator_task.h"
#include "api/mistral_types.h"
#include "trace/trace.h"

#include <cpptui.hpp>
#include <dripcore/scheduler.h>
#include <dripcore/channel.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace mistral::tui {

inline int g_sigint_pipe_write_fd = -1;

inline void sigint_handler(int)
{
  if (g_sigint_pipe_write_fd >= 0) {
    char buf = '1';
    ssize_t ret = ::write(g_sigint_pipe_write_fd, &buf, 1);
    (void)ret;
  }
}

class tui_app {
public:
  struct config {
    std::string agent_id;
    std::string host;
    int port;
    std::string api_key;
    std::string theme_name;
    std::string resolved_model;
    std::shared_ptr<dripcore::channel<std::string>> prompt_ch;
    std::shared_ptr<dripcore::channel<std::string>> input_ch;
    std::shared_ptr<dripcore::channel<mistral::api::api_request>> request_ch;
    std::shared_ptr<dripcore::channel<std::string>> agent_response_ch;
    std::shared_ptr<dripcore::channel<mistral::slash_command::slash_command_request>> cmd_request_ch;
    std::shared_ptr<dripcore::channel<std::string>> slash_cmd_response_ch;
  };

  explicit tui_app(config cfg)
    : cfg_(std::move(cfg))
  {
    if (::pipe(sigint_pipe_fds_) != 0) {
      throw std::runtime_error("failed to create signal pipe");
    }
    g_sigint_pipe_write_fd = sigint_pipe_fds_[1];
    mistral::agent::agent_system::instance().set_shutdown_pipe_fd(
      sigint_pipe_fds_[1]);

    struct sigaction sa{};
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);

    if (::pipe(input_pipe_fds_) != 0) {
      throw std::runtime_error("failed to create input pipe");
    }

    if (::pipe(slash_cmd_pipe_fds_) != 0) {
      throw std::runtime_error("failed to create slash command pipe");
    }

    std::freopen(".mistral-cli/tui-stderr.log", "w", stderr);

    build_widgets();
    wire_input();
    register_keybindings();
    setup_timers();
  }

  ~tui_app()
  {
    if (dripcore_thread_.joinable()) {
      shutdown();
    }
  }

  tui_app(const tui_app&) = delete;
  tui_app& operator=(const tui_app&) = delete;

  void run()
  {
    launch_dripcore();
    app_.run(root_);
    std::cout.flush();
  }

  void shutdown()
  {
    if (!dripcore_thread_.joinable()) return;

    char cancel_byte = '1';
    ::write(sigint_pipe_fds_[1], &cancel_byte, 1);

    signal(SIGINT, SIG_DFL);
    g_sigint_pipe_write_fd = -1;

    ::close(input_pipe_fds_[1]);
    ::close(slash_cmd_pipe_fds_[1]);
    ::close(sigint_pipe_fds_[1]);

    dripcore_thread_.join();

    ::close(input_pipe_fds_[0]);
    ::close(slash_cmd_pipe_fds_[0]);
    ::close(sigint_pipe_fds_[0]);
  }

private:
  using content_type = mistral::agent::content_type;

  void build_widgets()
  {
    using namespace cpptui;

    if (cfg_.theme_name == "light") {
      Theme::set_theme(Theme::Light());
    } else if (cfg_.theme_name == "nord") {
      Theme::set_theme(Theme::Nord());
    } else if (cfg_.theme_name == "tokyo-night") {
      Theme::set_theme(Theme::TokyoNight());
    } else if (cfg_.theme_name == "solarized-light") {
      Theme::set_theme(Theme::SolarizedLight());
    } else {
      Theme::set_theme(Theme::Dark());
    }

    dot_ = std::make_shared<Label>(
      StyledText("\xe2\x97\x8f"));
    dot_->fixed_width = 1;
    dot_->focusable = false;
    dot_->fg_color = Theme::current().border;

    header_ = std::make_shared<Horizontal>();
    header_->fixed_height = 1;
    header_->add(std::make_shared<HorizontalSpacer>(1));
    header_->add(std::make_shared<HorizontalSpacer>());
    header_->add(dot_);
    header_->add(std::make_shared<HorizontalSpacer>(1));

    content_ = std::make_shared<Paragraph>();
    content_->word_wrap = true;
    content_->preserve_whitespace = true;
    content_->fg_color = Theme::current().foreground;
    content_->bg_color = Theme::current().background;

    content_styled_.add(
      "mistral-tui prototype\n"
      "Type your message. Alt+Enter or double-Enter to send. "
      "ESC to cancel. Ctrl+C to quit.\n\n");
    content_->set_styled(content_styled_);

    {
      int w = content_->width > 0 ? content_->width : 80;
      auto lines = Paragraph::wrap_text_static(
        content_->text, w, content_->first_line_indent,
        content_->hanging_indent, content_->preserve_whitespace);
      content_->fixed_height = (int)lines.size();
    }

    scroll_ = std::make_shared<ScrollableVertical>();
    scroll_->add(content_);
    scroll_->focusable = false;

    input_row_ = std::make_shared<Horizontal>();
    input_row_->fixed_height = 5;

    input_ = std::make_shared<ChatInput>();
    input_->show_line_numbers = false;
    input_->word_wrap = true;
    input_->show_scrollbar = true;
    input_->accepts_tab = false;
    input_->fixed_height = 5;

    input_row_->add(std::make_shared<HorizontalSpacer>(1));
    input_row_->add(input_);

    input_hint_ = std::make_shared<InputHint>();
    input_hint_->fixed_height = 1;
    input_hint_->visible = false;

    input_divider_ = std::make_shared<Divider>();
    input_divider_->visible = false;

    input_divider_top_ = std::make_shared<Divider>();

    slash_divider_ = std::make_shared<Divider>();
    slash_divider_->visible = false;

    slash_popup_ = std::make_shared<SlashCommandPopup>();
    slash_popup_->fixed_height = 0;
    slash_popup_->visible = false;
    slash_popup_->set_registry(
      mistral::agent::agent_system::instance().slash_commands().get());

    footer_ = std::make_shared<FooterBar>();
    footer_->left_text = std::filesystem::current_path().string();
    footer_->center_text =
      cfg_.agent_id + " \xc2\xb7 " + cfg_.resolved_model;
    footer_->right_text = "0 tokens";
    footer_->fg_color = Theme::current().panel_fg;
    footer_->bg_color = Theme::current().panel_bg;

    root_ = std::make_shared<Vertical>();
    root_->width = 80;
    root_->height = 24;
    input_row_->width = 80;
    input_row_->height = 5;
    input_->width = 78;
    input_->height = 5;
    input_hint_->width = 80;
    input_hint_->height = 1;

    root_->add(header_);
    root_->add(std::make_shared<Divider>());
    root_->add(scroll_);
    root_->add(input_divider_);
    root_->add(input_hint_);
    root_->add(slash_divider_);
    root_->add(slash_popup_);
    root_->add(input_divider_top_);
    root_->add(input_row_);
    root_->add(std::make_shared<Divider>());
    root_->add(footer_);
  }

  void wire_input()
  {
    input_->on_submit = [this]() { send_input(); };
    input_->on_approval_key = [this](char ch) { on_approval_key(ch); };
    input_->on_approval_text_submit = [this]() {
      on_approval_text_submit();
    };

    input_->set_slash_popup(slash_popup_.get());
    input_->set_slash_divider(slash_divider_.get());
    input_->is_agent_busy = [this]() {
      return !agent_ready_.load(std::memory_order_acquire);
    };
    input_->on_slash_command =
      [this](std::string command, std::string args) {
        if (command == "exit") {
          app_.quit();
          return;
        }
        if (!agent_ready_.load(std::memory_order_acquire)) return;
        slash_cmd_queue_.push({command, args});
        char wakeup = '1';
        ::write(slash_cmd_pipe_fds_[1], &wakeup, 1);
      };
  }

  void register_keybindings()
  {
    app_.register_key(1001, [this]() {
      int visible_h = scroll_->height > 0 ? scroll_->height : 1;
      scroll_->scroll_offset -= visible_h;
      clamp_scroll();
    });

    app_.register_key(1002, [this]() {
      int visible_h = scroll_->height > 0 ? scroll_->height : 1;
      scroll_->scroll_offset += visible_h;
      clamp_scroll();
    });

    app_.register_key(1065, [this]() {
      scroll_->scroll_offset -= 1;
      clamp_scroll();
    }, false, true);

    app_.register_key(1066, [this]() {
      scroll_->scroll_offset += 1;
      clamp_scroll();
    }, false, true);

    app_.register_key(27, [this]() {
      if (input_->slash_mode_active()) {
        input_->cancel_slash_mode();
        return;
      }
      char cancel_byte = '1';
      ::write(g_sigint_pipe_write_fd, &cancel_byte, 1);
    });
  }

  void setup_timers()
  {
    app_.add_timer(50, [this]() {
      int state = approval_state_.load(std::memory_order_acquire);
      if (state == 1) {
        if (input_->approval_mode_ != ChatInput::approval_mode::single_key) {
          input_->set_approval_mode(ChatInput::approval_mode::single_key);
          input_divider_->visible = true;
          input_divider_->override_fg = cpptui::Theme::current().warning;
          input_divider_top_->override_fg = cpptui::Theme::current().warning;
          input_hint_->visible = true;
          input_hint_->text =
            "  approval \xc2\xb7 y=approve n=reject"
            " r=reject+msg a=all ?=help";
          input_hint_->hint_fg = cpptui::Theme::current().warning;

          if (content_at_bottom_) {
            if (auto cont =
                  std::dynamic_pointer_cast<cpptui::Container>(root_)) {
              cont->layout();
            }
            scroll_->scroll_offset = max_scroll_offset();
          }
        }
      } else if (state == 0) {
        if (input_->approval_mode_ != ChatInput::approval_mode::none) {
          exit_approval_mode();
        }
      }
    });

    app_.add_timer(200, [this]() {
      if (agent_ready_.load(std::memory_order_acquire)) {
        dot_->fg_color = cpptui::Theme::current().success;
      } else {
        dot_->fg_color = cpptui::Theme::current().border;
      }

      auto stats = mistral::agent::agent_system::instance().get_usage_stats();
      if (stats.contains("total_tokens")) {
        footer_->right_text =
          std::to_string(stats["total_tokens"].get<int>()) + " tokens";
      }
    });
  }

  void launch_dripcore()
  {
    dripcore_thread_ = std::thread([this]() {
      dripcore::scheduler sched;
      auto& agent_system = mistral::agent::agent_system::instance();

      auto post_fn = [this](std::function<void()> cb) {
        app_.post(std::move(cb));
      };
      auto append_content_fn =
        [this](const std::string& fragment,
               mistral::agent::content_type type) {
          append_content(fragment, type);
        };
      auto append_fn =
        [this](const std::string& fragment,
               mistral::agent::content_type type) {
          append_fragment(fragment, type);
        };
      auto flush_fn = [this]() { flush_content(); };

      sched.spawn<mistral::agent::agent_system_task>(cfg_.agent_response_ch);
      sched.spawn<mistral::agent::agent_system_stats_task>(
        agent_system.stats_ch());
      sched.spawn<mistral::net::http_coordinator_task>(
        cfg_.host, cfg_.port, cfg_.api_key, cfg_.request_ch);
      sched.spawn<mistral::tui::input_bridge_task>(
        input_pipe_fds_[0], cfg_.input_ch, std::ref(inq_));
      sched.spawn<mistral::slash_command::slash_command_executor_task>(
        cfg_.cmd_request_ch, cfg_.slash_cmd_response_ch,
        agent_system.slash_commands());
      sched.spawn<mistral::tui::slash_command_response_receiver>(
        cfg_.slash_cmd_response_ch, post_fn, append_content_fn);
      sched.spawn<mistral::tui::slash_command_bridge_task>(
        slash_cmd_pipe_fds_[0], cfg_.cmd_request_ch,
        cfg_.slash_cmd_response_ch, std::ref(slash_cmd_queue_));
      sched.spawn<mistral::tui::prompt_bridge_task>(
        cfg_.prompt_ch, std::ref(agent_ready_), std::ref(approval_state_),
        post_fn, append_content_fn);

      auto sink = std::make_shared<mistral::agent::tui_render_sink>(
        post_fn, append_fn, flush_fn);
      sched.spawn<mistral::agent::interactive_agent_task>(
        cfg_.agent_id, cfg_.input_ch, cfg_.agent_response_ch,
        cfg_.prompt_ch, sink);

      sched.spawn<mistral::common::signal_watcher_task>(
        sigint_pipe_fds_[0]);

      sched.run();
    });
  }

  void append_fragment(const std::string& fragment, content_type type)
  {
    using ct = content_type;
    switch (type) {
      case ct::normal:
        content_styled_.add(fragment);
        break;
      case ct::thinking:
        content_styled_.colored(
          fragment, cpptui::Theme::current().input_placeholder);
        break;
      case ct::tool_call:
        content_styled_.colored_bold(
          fragment, cpptui::Theme::current().primary);
        break;
      case ct::tool_result:
        content_styled_.colored(
          fragment, cpptui::Theme::current().primary);
        break;
      case ct::error:
        content_styled_.colored_bold(
          fragment, cpptui::Theme::current().error);
        break;
      case ct::warning:
        content_styled_.colored(
          fragment, cpptui::Theme::current().warning);
        break;
      case ct::user:
        content_styled_.colored_bold(
          fragment, cpptui::Theme::current().success);
        break;
      case ct::system:
        content_styled_.colored(
          fragment, cpptui::Theme::current().warning);
        break;
    }
  }

  void flush_content()
  {
    int visible_h = scroll_->height > 0 ? scroll_->height : 1;
    int ms = scroll_->content_height - visible_h;
    bool was_at_bottom = (scroll_->scroll_offset >= (ms < 0 ? 0 : ms));
    content_at_bottom_ = was_at_bottom;

    content_->set_styled(content_styled_);

    int wrap_width = content_->width > 0 ? content_->width : 80;
    auto lines = cpptui::Paragraph::wrap_text_static(
      content_->text, wrap_width, content_->first_line_indent,
      content_->hanging_indent, content_->preserve_whitespace);
    content_->fixed_height = (int)lines.size();

    if (was_at_bottom) {
      int max_scroll = (int)lines.size() - visible_h;
      if (max_scroll < 0) max_scroll = 0;
      scroll_->scroll_offset = max_scroll;
    }
  }

  void append_content(const std::string& fragment, content_type type)
  {
    append_fragment(fragment, type);
    flush_content();
  }

  void send_response(std::string response)
  {
    inq_.push(std::move(response));
    char wakeup = '1';
    ::write(input_pipe_fds_[1], &wakeup, 1);
  }

  void send_input()
  {
    if (approval_state_.load(std::memory_order_acquire) != 0) return;
    std::string text = input_->get_text();
    if (text.empty()) return;
    append_content("\n> " + text + "\n", content_type::user);
    input_->set_text("");
    agent_ready_.store(false, std::memory_order_release);
    send_response(std::move(text));
  }

  void exit_approval_mode()
  {
    approval_state_.store(0, std::memory_order_release);
    input_->set_approval_mode(ChatInput::approval_mode::none);
    input_divider_->visible = false;
    input_divider_->override_fg = cpptui::Color();
    input_divider_top_->override_fg = cpptui::Color();
    input_hint_->visible = false;
    input_hint_->text = "";
    input_hint_->hint_fg = cpptui::Color();
    app_.update();
  }

  void on_approval_key(char ch)
  {
    char lower = static_cast<char>(
      std::tolower(static_cast<unsigned char>(ch)));

    if (lower == 'r') {
      send_response("r");
      approval_state_.store(2, std::memory_order_release);
      input_->set_approval_mode(ChatInput::approval_mode::text_input);
      input_->set_text("");
      input_hint_->text =
        "  Enter message for AI (or Enter for default):";
      input_hint_->hint_fg = cpptui::Theme::current().warning;
      input_divider_top_->override_fg = cpptui::Theme::current().warning;
      mistral::trace::logger::instance().trace(
        mistral::trace::category::approval,
        "approval: switching to text_input mode"
      );
      return;
    }

    exit_approval_mode();
    send_response(std::string(1, lower));

    mistral::trace::logger::instance().trace(
      mistral::trace::category::approval,
      "approval: single-key response '{}'", lower
    );
  }

  void on_approval_text_submit()
  {
    std::string text = input_->get_text();
    exit_approval_mode();
    send_response(std::move(text));
    mistral::trace::logger::instance().trace(
      mistral::trace::category::approval,
      "approval: text response sent"
    );
  }

  int max_scroll_offset()
  {
    int visible_h = scroll_->height > 0 ? scroll_->height : 1;
    int ms = scroll_->content_height - visible_h;
    return ms < 0 ? 0 : ms;
  }

  void clamp_scroll()
  {
    if (scroll_->scroll_offset < 0) scroll_->scroll_offset = 0;
    int ms = max_scroll_offset();
    if (scroll_->scroll_offset > ms) scroll_->scroll_offset = ms;
  }

  config cfg_;

  cpptui::App app_;
  std::shared_ptr<cpptui::Label> dot_;
  std::shared_ptr<cpptui::Horizontal> header_;
  std::shared_ptr<cpptui::Paragraph> content_;
  std::shared_ptr<cpptui::ScrollableVertical> scroll_;
  std::shared_ptr<ChatInput> input_;
  std::shared_ptr<cpptui::Horizontal> input_row_;
  std::shared_ptr<InputHint> input_hint_;
  std::shared_ptr<Divider> input_divider_;
  std::shared_ptr<Divider> input_divider_top_;
  std::shared_ptr<Divider> slash_divider_;
  std::shared_ptr<SlashCommandPopup> slash_popup_;
  std::shared_ptr<FooterBar> footer_;
  std::shared_ptr<cpptui::Vertical> root_;
  cpptui::StyledText content_styled_;
  bool content_at_bottom_ = true;

  thread_safe_queue<std::string> inq_;
  thread_safe_queue<mistral::slash_command::slash_command_request>
    slash_cmd_queue_;
  std::atomic<bool> agent_ready_{true};
  std::atomic<int> approval_state_{0};

  int sigint_pipe_fds_[2] = {-1, -1};
  int input_pipe_fds_[2] = {-1, -1};
  int slash_cmd_pipe_fds_[2] = {-1, -1};

  std::thread dripcore_thread_;
};

} // namespace mistral::tui

#endif // MISTRAL_TUI_APP_H