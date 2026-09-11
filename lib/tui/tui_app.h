#ifndef MISTRAL_TUI_APP_H
#define MISTRAL_TUI_APP_H

#include "tui/tui_content_area.h"
#include "tui/tui_backend.h"
#include "tui/tui_widgets.h"
#include "tui/tui_approval_state.h"
#include "agent/agent_system.h"
#include "trace/trace.h"

#include <cpptui.hpp>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>

namespace mistral::tui {

class tui_app {
public:
  explicit tui_app(tui_config cfg)
    : cfg_(std::move(cfg))
  {
    std::freopen(".mistral-cli/tui-stderr.log", "w", stderr);

    build_widgets();

    backend_ = std::make_unique<tui_backend>(
      cfg_,
      tui_backend::ui_callbacks{
        [this](std::function<void()> cb) {
          app_.post(std::move(cb));
        },
        [this](const std::string& fragment,
               mistral::agent::content_type type) {
          content_area_.append(fragment, type);
        },
        [this](const std::string& fragment,
               mistral::agent::content_type type) {
          content_area_.append_fragment(fragment, type);
        },
        [this]() { content_area_.flush(); },
        [this]() { content_area_.complete_turn(); },
        [this](approval_state state) { set_approval_state(state); }
      }
    );

    wire_input();
    register_keybindings();
    setup_timers();
  }

  ~tui_app()
  {
    if (backend_) {
      backend_->shutdown();
    }
  }

  tui_app(const tui_app&) = delete;
  tui_app& operator=(const tui_app&) = delete;

  void run()
  {
    backend_->start();
    app_.run(root_);
    std::cout.flush();
  }

  void shutdown()
  {
    if (backend_) backend_->shutdown();
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

    auto scroll_widget = content_area_.build();

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
    root_->add(scroll_widget);
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
      return !backend_->agent_ready().load(std::memory_order_acquire);
    };
    input_->on_slash_command =
      [this](std::string command, std::string args) {
        if (command == "exit") {
          app_.quit();
          return;
        }
        if (!backend_->agent_ready().load(std::memory_order_acquire)) return;
        backend_->send_slash_command({command, args});
      };
  }

  void register_keybindings()
  {
    app_.register_key(1001, [this]() {
      content_area_.scroll_page_up();
    });

    app_.register_key(1002, [this]() {
      content_area_.scroll_page_down();
    });

    app_.register_key(1065, [this]() {
      content_area_.scroll_line_up();
    }, false, true);

    app_.register_key(1066, [this]() {
      content_area_.scroll_line_down();
    }, false, true);

    app_.register_key(27, [this]() {
      if (input_->slash_mode_active()) {
        input_->cancel_slash_mode();
        return;
      }
      if (input_->approval_mode_ != ChatInput::approval_mode::none) {
        // ESC during approval mode: exit approval mode and send a
        // reject response so the agent can continue.
        if (input_->approval_mode_ == ChatInput::approval_mode::text_input) {
          // In text_input mode the agent is waiting for the rejection
          // message. Send an empty string so the agent uses the default
          // rejection message.
          exit_approval_mode();
          backend_->send_input("");
        } else {
          // In single_key mode the agent is waiting for the approval
          // response. Send 'n' (reject).
          exit_approval_mode();
          backend_->send_input("n");
        }
        mistral::trace::logger::instance().trace(
          mistral::trace::category::approval,
          "approval: ESC pressed, exiting approval mode"
        );
        return;
      }
      char cancel_byte = '1';
      ::write(backend_->sigint_write_fd(), &cancel_byte, 1);
    });
  }

  void setup_timers()
  {
    app_.add_timer(200, [this]() {
      if (backend_->agent_ready().load(std::memory_order_acquire)) {
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

  void send_input()
  {
    // Block normal submission while in approval mode. Approval
    // responses are handled by on_approval_key / on_approval_text_submit.
    if (input_->approval_mode_ != ChatInput::approval_mode::none) {
      return;
    }
    std::string text = input_->get_text();
    if (text.empty()) return;
    content_area_.append("\n> " + text + "\n", content_type::user);
    input_->set_text("");
    backend_->agent_ready().store(false, std::memory_order_release);
    backend_->send_input(std::move(text));
  }

  void exit_approval_mode()
  {
    set_approval_state(approval_state::idle);
  }

  // Consolidated approval state transition: updates ChatInput mode,
  // divider visibility, hint text, and scroll position in one place.
  // Called from:
  //   - prompt_bridge_task (via posted callback) for single_key
  //   - on_approval_key for text_input ('r' key)
  //   - exit_approval_mode for idle
  void set_approval_state(approval_state state)
  {
    ChatInput::approval_mode mode = ChatInput::approval_mode::none;
    switch (state) {
      case approval_state::single_key:
        mode = ChatInput::approval_mode::single_key;
        break;
      case approval_state::text_input:
        mode = ChatInput::approval_mode::text_input;
        break;
      case approval_state::idle:
        break;
    }

    if (mode == input_->approval_mode_) {
      return;
    }

    input_->set_approval_mode(mode);

    switch (state) {
      case approval_state::idle:
        input_divider_->visible = false;
        input_divider_->override_fg = cpptui::Color();
        input_divider_top_->override_fg = cpptui::Color();
        input_hint_->visible = false;
        input_hint_->text = "";
        input_hint_->hint_fg = cpptui::Color();
        break;

      case approval_state::single_key:
        input_divider_->visible = true;
        input_divider_->override_fg = cpptui::Theme::current().warning;
        input_divider_top_->override_fg = cpptui::Theme::current().warning;
        input_hint_->visible = true;
        input_hint_->hint_fg = cpptui::Theme::current().warning;
        input_hint_->text =
          "  approval \xc2\xb7 y=approve n=reject"
          " r=reject+msg a=all ?=help";

        if (content_area_.was_at_bottom()) {
          if (auto cont =
                std::dynamic_pointer_cast<cpptui::Container>(root_)) {
            cont->layout();
          }
          content_area_.scroll_to_bottom();
        }
        break;

      case approval_state::text_input:
        input_divider_->visible = true;
        input_divider_->override_fg = cpptui::Theme::current().warning;
        input_divider_top_->override_fg = cpptui::Theme::current().warning;
        input_hint_->visible = true;
        input_hint_->hint_fg = cpptui::Theme::current().warning;
        input_->set_text("");
        input_hint_->text =
          "  Enter message for AI (or Enter for default):";
        break;
    }

    app_.update();
  }

  void on_approval_key(char ch)
  {
    char lower = static_cast<char>(
      std::tolower(static_cast<unsigned char>(ch)));

    if (lower == 'r') {
      backend_->send_input("r");
      set_approval_state(approval_state::text_input);
      mistral::trace::logger::instance().trace(
        mistral::trace::category::approval,
        "approval: switching to text_input mode"
      );
      return;
    }

    exit_approval_mode();
    backend_->send_input(std::string(1, lower));

    mistral::trace::logger::instance().trace(
      mistral::trace::category::approval,
      "approval: single-key response '{}'", lower
    );
  }

  void on_approval_text_submit()
  {
    std::string text = input_->get_text();
    exit_approval_mode();
    backend_->send_input(std::move(text));
    mistral::trace::logger::instance().trace(
      mistral::trace::category::approval,
      "approval: text response sent"
    );
  }

  tui_config cfg_;

  cpptui::App app_;
  content_area content_area_;
  std::unique_ptr<tui_backend> backend_;
  std::shared_ptr<cpptui::Label> dot_;
  std::shared_ptr<cpptui::Horizontal> header_;
  std::shared_ptr<ChatInput> input_;
  std::shared_ptr<cpptui::Horizontal> input_row_;
  std::shared_ptr<InputHint> input_hint_;
  std::shared_ptr<Divider> input_divider_;
  std::shared_ptr<Divider> input_divider_top_;
  std::shared_ptr<Divider> slash_divider_;
  std::shared_ptr<SlashCommandPopup> slash_popup_;
  std::shared_ptr<FooterBar> footer_;
  std::shared_ptr<cpptui::Vertical> root_;
};

} // namespace mistral::tui

#endif // MISTRAL_TUI_APP_H