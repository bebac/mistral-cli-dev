#ifndef MISTRAL_TUI_APP_H
#define MISTRAL_TUI_APP_H

#include "agent/agent_system.h"
#include "mcp/mcp_manager.h"
#include "trace/trace.h"
#include "tui/tui_approval_state.h"
#include "tui/tui_backend.h"
#include "tui/tui_chat_input.h"
#include "tui/tui_content_area.h"
#include "tui/tui_slash_command_popup.h"
#include "tui/tui_widgets.h"

#include <cpptui.hpp>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>

namespace mistral::tui
{

  class tui_app
  {
  public:
    explicit tui_app(tui_config cfg) : cfg_(std::move(cfg))
    {
      std::freopen(".mistral-cli/tui-stderr.log", "w", stderr);

      build_widgets();

      backend_ = std::make_unique<tui_backend>(
        cfg_,
        tui_backend::ui_callbacks{
          [this](std::function<void()> cb) { app_.post(std::move(cb)); },
          [this](const std::string& fragment, mistral::tui::content_type type) {
            content_area_.append(fragment, type);
          },
          [this](const std::string& fragment, mistral::tui::content_type type) {
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
      if (backend_)
      {
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
      if (backend_)
        backend_->shutdown();
    }

  private:
    using content_type = mistral::tui::content_type;

    void build_widgets()
    {
      using namespace cpptui;

      if (cfg_.theme_name == "light")
      {
        Theme::set_theme(Theme::Light());
      }
      else if (cfg_.theme_name == "nord")
      {
        Theme::set_theme(Theme::Nord());
      }
      else if (cfg_.theme_name == "tokyo-night")
      {
        Theme::set_theme(Theme::TokyoNight());
      }
      else if (cfg_.theme_name == "solarized-light")
      {
        Theme::set_theme(Theme::SolarizedLight());
      }
      else
      {
        Theme::set_theme(Theme::Dark());
      }

      dot_ = std::make_shared<Label>(StyledText("\xe2\x97\x8f"));
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
      slash_popup_->set_registry(mistral::agent::agent_system::instance().slash_commands().get());

      footer_ = std::make_shared<FooterBar>();
      footer_->left_text = std::filesystem::current_path().string();
      footer_->center_text = cfg_.agent_id + " \xc2\xb7 " + cfg_.resolved_model;
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
      input_->on_approval_text_submit = [this]() { on_approval_text_submit(); };

      input_->set_slash_popup(slash_popup_.get());
      input_->set_slash_divider(slash_divider_.get());
      input_->is_agent_busy = [this]() { return !backend_->agent_ready().load(std::memory_order_acquire); };
      input_->on_slash_command = [this](std::string command, std::string args) {
        if (command == "exit")
        {
          app_.quit();
          return;
        }
        if (!backend_->agent_ready().load(std::memory_order_acquire))
          return;
        backend_->send_slash_command({command, args});
      };
    }

    void register_keybindings()
    {
      app_.register_key(1001, [this]() { content_area_.scroll_page_up(); });

      app_.register_key(1002, [this]() { content_area_.scroll_page_down(); });

      app_.register_key(1065, [this]() { content_area_.scroll_line_up(); }, false, true);

      app_.register_key(1066, [this]() { content_area_.scroll_line_down(); }, false, true);

      app_.register_key(27, [this]() {
        if (input_->slash_mode_active())
        {
          input_->cancel_slash_mode();
          return;
        }
        if (input_->approval_mode_ != ChatInput::approval_mode::none)
        {
          // ESC during approval/ask mode: exit the mode and send a
          // response so the agent can continue.
          if (input_->approval_mode_ == ChatInput::approval_mode::text_input)
          {
            // In text_input mode the agent is waiting for free text: a
            // rejection message (approval flow) or an answer (ask_user).
            // Send an empty string: the approval flow uses the default
            // rejection message, ask_user treats it as "skipped".
            exit_approval_mode();
            backend_->send_input("");
          }
          else
          {
            // In single_key mode the agent is waiting for the approval
            // response. Send 'n' (reject).
            exit_approval_mode();
            backend_->send_input("n");
          }
          mistral::trace::logger::instance().trace(
            mistral::trace::category::approval, "approval: ESC pressed, exiting approval mode"
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
        if (backend_->agent_ready().load(std::memory_order_acquire))
        {
          dot_->fg_color = cpptui::Theme::current().success;
        }
        else
        {
          dot_->fg_color = cpptui::Theme::current().border;
        }

        // Update footer center with MCP auto-connect status.
        auto& mcp_mgr = mistral::mcp::mcp_manager::instance();
        auto mcp_servers = mcp_mgr.auto_connect_servers();
        if (!mcp_servers.empty())
        {
          std::string names;
          for (size_t i = 0; i < mcp_servers.size(); ++i)
          {
            if (i > 0)
              names += " ";
            names += mcp_servers[i];
          }
          bool all_connected = mcp_mgr.auto_connect_all_connected();
          std::string indicator = all_connected ? "\xe2\x97\x8f" : "\xe2\x97\x8b";
          footer_->center_text =
            cfg_.agent_id + " \xc2\xb7 " + cfg_.resolved_model + " \xc2\xb7 " + names + " " + indicator;

          // Show a single status message in the content area (once).
          // Connection status is shown in the footer dot; once the
          // auto-connect task has finished, failures and preflight
          // warnings are printed (once) — success stays silent.
          if (mcp_status_msg_state_ == 0)
          {
            content_area_.append("Running MCP auto-connect...", content_type::system);
            mcp_status_msg_state_ = 1;
          }
          else if (mcp_status_msg_state_ == 1 && mcp_mgr.auto_connect_finished())
          {
            std::string errors;
            std::string warnings;
            for (const auto& r : mcp_mgr.auto_connect_report())
            {
              if (r.contains("error"))
              {
                errors += std::format("  {}: {}\n", r["server"].get<std::string>(), r["error"].get<std::string>());
              }
              if (r.contains("warning"))
              {
                warnings += std::format("  {}: {}\n", r["server"].get<std::string>(), r["warning"].get<std::string>());
              }
            }
            if (!errors.empty())
            {
              content_area_.append("MCP auto-connect failed:\n" + errors, content_type::error);
            }
            if (!warnings.empty())
            {
              content_area_.append("MCP preflight warning:\n" + warnings, content_type::warning);
            }
            mcp_status_msg_state_ = 2;
          }
        }
        else
        {
          footer_->center_text = cfg_.agent_id + " \xc2\xb7 " + cfg_.resolved_model;
        }

        auto stats = mistral::agent::agent_system::instance().get_usage_stats();
        if (stats.contains("total_tokens"))
        {
          footer_->right_text = std::to_string(stats["total_tokens"].get<int>()) + " tokens";
        }
      });
    }

    void send_input()
    {
      // Block normal submission while in approval mode. Approval
      // responses are handled by on_approval_key / on_approval_text_submit.
      if (input_->approval_mode_ != ChatInput::approval_mode::none)
      {
        return;
      }
      // get_submit_text(): trailing whitespace (the blank confirmation
      // line of a double-Enter submit, stray trailing spaces) is not sent.
      std::string text = input_->get_submit_text();
      if (text.empty())
        return;
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
    //   - prompt_bridge_task (via posted callback) for single_key and ask
    //   - on_approval_key for text_input ('r' key)
    //   - exit_approval_mode for idle
    void set_approval_state(approval_state state)
    {
      ChatInput::approval_mode mode = ChatInput::approval_mode::none;
      switch (state)
      {
      case approval_state::single_key:
        mode = ChatInput::approval_mode::single_key;
        break;
      case approval_state::text_input:
      case approval_state::ask:
        mode = ChatInput::approval_mode::text_input;
        break;
      case approval_state::idle:
        break;
      }

      if (mode == input_->approval_mode_)
      {
        return;
      }

      input_->set_approval_mode(mode);

      switch (state)
      {
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
        input_hint_->text = "  approval \xc2\xb7 y=approve n=reject"
                            " r=reject+msg a=all ?=help";

        // The approval prompt is blocking: the agent is waiting for the
        // user's response. Always jump to the bottom so the prompt is
        // visible even if the user had scrolled up — otherwise it is
        // easy to miss that the agent is waiting for approval.
        if (auto cont = std::dynamic_pointer_cast<cpptui::Container>(root_))
        {
          cont->layout();
        }
        content_area_.scroll_to_bottom();

        mistral::trace::logger::instance().trace(
          mistral::trace::category::approval, "approval: single_key mode entered, scrolled to bottom"
        );
        break;

      case approval_state::text_input:
        input_divider_->visible = true;
        input_divider_->override_fg = cpptui::Theme::current().warning;
        input_divider_top_->override_fg = cpptui::Theme::current().warning;
        input_hint_->visible = true;
        input_hint_->hint_fg = cpptui::Theme::current().warning;
        input_->set_text("");
        input_hint_->text = "  Enter message for AI (or Enter for default):";
        break;

      case approval_state::ask:
        input_divider_->visible = true;
        input_divider_->override_fg = cpptui::Theme::current().warning;
        input_divider_top_->override_fg = cpptui::Theme::current().warning;
        input_hint_->visible = true;
        input_hint_->hint_fg = cpptui::Theme::current().warning;
        input_->set_text("");
        input_hint_->text = "  answer \xc2\xb7 Enter to send \xc2\xb7 Esc to skip";

        // The ask prompt is blocking: the agent is waiting for the
        // human's answer. Always jump to the bottom so the question is
        // visible even if the user had scrolled up.
        if (auto cont = std::dynamic_pointer_cast<cpptui::Container>(root_))
        {
          cont->layout();
        }
        content_area_.scroll_to_bottom();

        mistral::trace::logger::instance().trace(
          mistral::trace::category::approval, "ask: answer mode entered, scrolled to bottom"
        );
        break;
      }

      app_.update();
    }

    void on_approval_key(char ch)
    {
      char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

      if (lower == 'r')
      {
        backend_->send_input("r");
        set_approval_state(approval_state::text_input);
        mistral::trace::logger::instance().trace(
          mistral::trace::category::approval, "approval: switching to text_input mode"
        );
        return;
      }

      exit_approval_mode();
      backend_->send_input(std::string(1, lower));

      mistral::trace::logger::instance().trace(
        mistral::trace::category::approval, "approval: single-key response '{}'", lower
      );
    }

    void on_approval_text_submit()
    {
      std::string text = input_->get_text();
      exit_approval_mode();
      backend_->send_input(std::move(text));
      mistral::trace::logger::instance().trace(mistral::trace::category::approval, "approval: text response sent");
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

    // MCP auto-connect content-area message state:
    // 0 = "Running MCP auto-connect..." not shown yet,
    // 1 = running message shown, waiting for the outcome,
    // 2 = outcome shown (failure/warning lines, if any)
    int mcp_status_msg_state_ = 0;
  };

} // namespace mistral::tui

#endif // MISTRAL_TUI_APP_H
