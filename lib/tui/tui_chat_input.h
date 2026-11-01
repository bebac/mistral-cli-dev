#ifndef MISTRAL_TUI_CHAT_INPUT_H
#define MISTRAL_TUI_CHAT_INPUT_H

#include "slash_command/slash_command_registry.h"
#include "tui/tui_slash_command_popup.h"
#include "tui/tui_widgets.h"

#include "cpptui.hpp"

#include <cctype>
#include <functional>
#include <string>
#include <utility>

namespace mistral::tui
{

  // ---------------------------------------------------------------------------
  // ChatInput: multiline input area based on TextArea.
  // Enter inserts a newline. Alt+Enter or Enter on an empty or
  // whitespace-only last line (double-Enter) submits the message.
  //
  // Approval mode state machine:
  //   none        → normal chat input
  //   single_key  → intercepts y/n/r/a/? before TextArea processes them
  //   text_input  → normal editing for reject-with-message, Enter submits
  // ---------------------------------------------------------------------------

  class ChatInput : public cpptui::TextArea
  {
  public:
    enum class approval_mode
    {
      none,
      single_key,
      text_input
    };

    std::function<void()> on_submit;
    std::function<void(char)> on_approval_key;
    std::function<void()> on_approval_text_submit;

    approval_mode approval_mode_ = approval_mode::none;

    // Slash command mode
    std::function<void(std::string, std::string)> on_slash_command;
    std::function<bool()> is_agent_busy;

    void set_slash_popup(SlashCommandPopup* popup)
    {
      popup_ = popup;
    }
    void set_slash_divider(Divider* divider)
    {
      slash_divider_ = divider;
    }
    void set_slash_registry(const mistral::slash_command::slash_command_registry* registry)
    {
      registry_ = registry;
    }
    bool slash_mode_active() const
    {
      return slash_mode_active_;
    }

    void cancel_slash_mode()
    {
      exit_slash_mode();
      set_text("");
    }

    // Text to send on submit: get_text() with trailing whitespace removed.
    // The blank last line created by the double-Enter confirmation and any
    // stray trailing spaces are not part of the message.
    std::string get_submit_text() const
    {
      std::string text = get_text();
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
      {
        text.pop_back();
      }
      return text;
    }

    void set_approval_mode(approval_mode mode)
    {
      approval_mode_ = mode;
      if (mode == approval_mode::none)
      {
        set_text("");
      }
      else if (mode == approval_mode::single_key)
      {
        set_text("");
      }
      // text_input mode: keep existing text, user is typing rejection message
    }

    bool on_event(const cpptui::Event& event) override
    {
      if (approval_mode_ == approval_mode::single_key)
      {
        // Intercept single-key approval responses before TextArea processes them.
        if (event.is_key_event() && event.is_printable() && !event.ctrl && !event.alt && !event.shift)
        {
          char ch = static_cast<char>(event.key);
          if (
            ch == 'y' || ch == 'n' || ch == 'r' || ch == 'a' || ch == '?' || ch == 'Y' || ch == 'N' || ch == 'R' ||
            ch == 'A'
          )
          {
            if (on_approval_key)
              on_approval_key(ch);
            return true;
          }
        }
        // Consume all other key events in single_key mode to prevent
        // TextArea from inserting characters.
        if (event.is_key_event())
        {
          return true;
        }
        return TextArea::on_event(event);
      }

      if (approval_mode_ == approval_mode::text_input)
      {
        // In text_input mode, Enter submits the rejection message.
        if (event.is_key_event() && event.is_enter())
        {
          if (!event.ctrl && !event.shift)
          {
            // Any Enter submits in text_input mode (no double-Enter needed).
            if (on_approval_text_submit)
              on_approval_text_submit();
            return true;
          }
        }
        // Let TextArea handle all other events (typing, backspace, etc.)
        return TextArea::on_event(event);
      }

      // Slash command mode: intercept navigation, tab, enter.
      if (approval_mode_ == approval_mode::none && slash_mode_active_)
      {
        if (event.is_key_event() && event.is_nav_up() && !event.shift && !event.ctrl)
        {
          if (popup_)
            popup_->move_selection(-1);
          return true;
        }
        if (event.is_key_event() && event.is_nav_down() && !event.shift && !event.ctrl)
        {
          if (popup_)
            popup_->move_selection(1);
          return true;
        }
        if (event.is_key_event() && event.is_tab())
        {
          // Tab only completes the current token; it never executes.
          std::string selected = popup_ ? popup_->selected_text() : "";
          if (!selected.empty())
          {
            accept_candidate(selected);
          }
          else
          {
            exit_slash_mode();
          }
          return true;
        }
        if (event.is_key_event() && event.is_enter() && !event.ctrl && !event.shift)
        {
          // Execute-on-complete: Enter runs the line as typed when it is
          // complete, and otherwise accepts the highlighted candidate.
          if (line_complete_)
          {
            submit_slash_command();
            return true;
          }
          std::string selected = popup_ ? popup_->selected_text() : "";
          if (!selected.empty())
          {
            accept_candidate(selected);
            // Accepting the last token can complete the line: run it.
            if (!slash_mode_active_ && line_complete_)
            {
              submit_slash_command();
            }
            return true;
          }
          exit_slash_mode();
          return true;
        }
      }

      // Normal chat mode: existing Enter handling.
      if (event.is_key_event() && event.is_enter())
      {
        // Slash command submission: when not in slash popup mode and
        // text starts with '/', single Enter executes the command
        // via on_slash_command (no double-Enter needed).
        if (!event.ctrl && !event.shift && !slash_mode_active_ && on_slash_command)
        {
          std::string text = get_text();
          if (!text.empty() && text[0] == '/')
          {
            submit_slash_command();
            return true;
          }
        }

        // Alt+Enter always submits.
        if (event.alt)
        {
          if (on_submit)
            on_submit();
          return true;
        }

        // Enter on an empty or whitespace-only last line with content
        // above submits (double-Enter).
        //
        // The whitespace-only case matters: with a stray space after the
        // cursor on the last line, every Enter splits the line at the
        // cursor and the space stays on the new last line. The last line
        // would never be empty and double-Enter could never submit.
        if (!event.ctrl && !event.shift)
        {
          bool is_last_line = (cursor_y_ == (int)lines_.size() - 1);
          bool current_line_blank = is_blank(lines_[cursor_y_]);
          bool has_content = false;
          for (const auto& l : lines_)
          {
            if (!is_blank(l))
            {
              has_content = true;
              break;
            }
          }
          if (is_last_line && current_line_blank && has_content)
          {
            if (on_submit)
              on_submit();
            return true;
          }
        }
      }

      bool consumed = TextArea::on_event(event);

      // After TextArea processes, recompute the completion state.
      if (approval_mode_ == approval_mode::none)
      {
        refresh_slash_state();
      }

      return consumed;
    }

  private:
    // A line that is empty or contains only whitespace. get_submit_text()
    // strips trailing whitespace, so such lines carry no message content.
    static bool is_blank(const std::string& line)
    {
      for (char c : line)
      {
        if (!std::isspace(static_cast<unsigned char>(c)))
          return false;
      }
      return true;
    }

    // Start of the token being typed. The text always starts with '/', so
    // the command-name token starts at index 1.
    static size_t last_token_start(const std::string& text)
    {
      size_t space = text.find_last_of(" \t");
      if (space == std::string::npos)
      {
        return 1;
      }
      return space + 1;
    }

    // Recompute the completion candidates for the current text and show or
    // hide the popup accordingly.
    void refresh_slash_state()
    {
      if (!registry_)
      {
        exit_slash_mode();
        return;
      }

      const std::string text = get_text();
      if (text.empty() || text[0] != '/' || text.find('\n') != std::string::npos)
      {
        exit_slash_mode();
        return;
      }

      mistral::slash_command::completion_state state = registry_->complete(text.substr(1));
      line_complete_ = state.complete;

      if (state.items.empty())
      {
        // Nothing to offer: get out of the way so Enter submits normally.
        exit_slash_mode();
        return;
      }

      if (!slash_mode_active_ && is_agent_busy && is_agent_busy())
      {
        exit_slash_mode();
        return;
      }

      slash_mode_active_ = true;
      if (popup_)
      {
        popup_->set_display_prefix(state.command_names ? "/" : "");
        popup_->set_items(std::move(state.items));
        popup_->fixed_height = popup_->popup_height();
        popup_->visible = true;
      }
      if (slash_divider_)
      {
        slash_divider_->visible = true;
      }
    }

    // Replace the token being typed with `candidate` and append a space.
    void accept_candidate(const std::string& candidate)
    {
      const std::string text = get_text();
      const size_t start = last_token_start(text);
      set_text(text.substr(0, start) + candidate + " ");
      // set_text resets cursor to 0; move it to end of line.
      cursor_x_ = (int)cpptui::prepare_text_for_render(lines_[cursor_y_]).size();
      ensure_cursor_visible();
      refresh_slash_state();
    }

    // Send the text as a slash command and clear the input.
    void submit_slash_command()
    {
      const std::string text = get_text();
      exit_slash_mode();
      if (text.empty() || text[0] != '/')
      {
        return;
      }

      std::string after_slash = text.substr(1);
      size_t space_pos = after_slash.find(' ');
      std::string cmd = (space_pos == std::string::npos) ? after_slash : after_slash.substr(0, space_pos);
      std::string args = (space_pos == std::string::npos) ? "" : after_slash.substr(space_pos + 1);
      // Trim trailing whitespace from args.
      while (!args.empty() && args.back() == ' ')
      {
        args.pop_back();
      }
      set_text("");
      if (on_slash_command)
      {
        on_slash_command(cmd, args);
      }
    }

    void exit_slash_mode()
    {
      slash_mode_active_ = false;
      if (popup_)
        popup_->visible = false;
      if (slash_divider_)
        slash_divider_->visible = false;
    }

    bool slash_mode_active_ = false;
    bool line_complete_ = false;
    SlashCommandPopup* popup_ = nullptr;
    Divider* slash_divider_ = nullptr;
    const mistral::slash_command::slash_command_registry* registry_ = nullptr;
  };

} // namespace mistral::tui

#endif // MISTRAL_TUI_CHAT_INPUT_H
