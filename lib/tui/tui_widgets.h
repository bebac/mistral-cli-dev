#ifndef MISTRAL_TUI_WIDGETS_H
#define MISTRAL_TUI_WIDGETS_H

#include "cpptui.hpp"
#include "console/slash_command_registry.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Divider: renders a horizontal line across full width.
// ---------------------------------------------------------------------------
class Divider : public cpptui::Widget {
public:
  Divider() { fixed_height = 1; }

  // When non-default, overrides the theme border color.
  cpptui::Color override_fg;

  void render(cpptui::Buffer& buffer) override {
    cpptui::Color fg = override_fg.is_default
      ? fg_color.resolve(cpptui::Theme::current().border)
      : override_fg;
    cpptui::Color bg = bg_color.resolve(cpptui::Theme::current().background);
    for (int i = 0; i < width; ++i) {
      cpptui::Cell c;
      c.content = "\xe2\x94\x80";  // ─ U+2500
      c.fg_color = fg;
      c.bg_color = bg;
      buffer.set(x + i, y, c);
    }
  }
};

// ---------------------------------------------------------------------------
// FooterBar: renders three strings at left, center, and right positions.
// ---------------------------------------------------------------------------
class FooterBar : public cpptui::Widget {
public:
  FooterBar() { fixed_height = 1; }

  std::string left_text;
  std::string center_text;
  std::string right_text;

  void render(cpptui::Buffer& buffer) override {
    cpptui::Color fg = fg_color.resolve(cpptui::Theme::current().panel_fg);
    cpptui::Color bg = bg_color.resolve(cpptui::Theme::current().panel_bg);

    // Fill background
    for (int i = 0; i < width; ++i) {
      cpptui::Cell c;
      c.content = " ";
      c.fg_color = fg;
      c.bg_color = bg;
      buffer.set(x + i, y, c);
    }

    auto render_text = [&](const std::string& text, int start_x) {
      auto chars = cpptui::TextHelper::prepare_text_for_render(text);
      int dx = 0;
      for (const auto& ci : chars) {
        if (start_x + dx >= width) break;
        cpptui::Cell c;
        c.content = ci.content;
        c.fg_color = fg;
        c.bg_color = bg;
        buffer.set(x + start_x + dx, y, c);
        if (ci.display_width == 2 && start_x + dx + 1 < width) {
          cpptui::Cell skip;
          skip.content = "";
          skip.bg_color = bg;
          buffer.set(x + start_x + dx + 1, y, skip);
        }
        dx += ci.display_width;
      }
    };

    // Left (1px left margin)
    render_text(left_text, 1);

    // Center (within inner width, offset by 1px left margin)
    int inner_w = width - 2;
    auto center_chars = cpptui::TextHelper::prepare_text_for_render(center_text);
    int center_w = 0;
    for (const auto& ci : center_chars) center_w += ci.display_width;
    render_text(center_text, 1 + (inner_w - center_w) / 2);

    // Right (1px right margin)
    auto right_chars = cpptui::TextHelper::prepare_text_for_render(right_text);
    int right_w = 0;
    for (const auto& ci : right_chars) right_w += ci.display_width;
    render_text(right_text, width - 1 - right_w);
  }
};

// ---------------------------------------------------------------------------
// InputHint: 1-line widget showing approval key bindings.
// Empty in normal chat mode; shows hints when approval mode is active.
// ---------------------------------------------------------------------------
class InputHint : public cpptui::Widget {
public:
  InputHint() { fixed_height = 1; }

  std::string text;
  cpptui::Color hint_fg;

  void render(cpptui::Buffer& buffer) override {
    cpptui::Color fg = hint_fg.is_default
      ? fg_color.resolve(cpptui::Theme::current().foreground)
      : hint_fg;
    cpptui::Color bg = bg_color.resolve(cpptui::Theme::current().background);

    // Fill background
    for (int i = 0; i < width; ++i) {
      cpptui::Cell c;
      c.content = " ";
      c.fg_color = fg;
      c.bg_color = bg;
      buffer.set(x + i, y, c);
    }

    if (text.empty()) return;

    // Render hint text with 1px left margin
    auto chars = cpptui::TextHelper::prepare_text_for_render(text);
    int dx = 0;
    for (const auto& ci : chars) {
      if (1 + dx >= width) break;
      cpptui::Cell c;
      c.content = ci.content;
      c.fg_color = fg;
      c.bg_color = bg;
      buffer.set(x + 1 + dx, y, c);
      if (ci.display_width == 2 && 1 + dx + 1 < width) {
        cpptui::Cell skip;
        skip.content = "";
        skip.bg_color = bg;
        buffer.set(x + 1 + dx + 1, y, skip);
      }
      dx += ci.display_width;
    }
  }
};

// ---------------------------------------------------------------------------
// SlashCommandPopup: bordered popup showing filtered slash commands.
// Renders a 2-column table: /command_name | description.
// Selected row highlighted with Theme::selection background.
// ---------------------------------------------------------------------------

class SlashCommandPopup : public cpptui::Widget {
public:
  static constexpr int max_visible = 8;
  static constexpr int col1_width = 20;

  SlashCommandPopup() { focusable = false; }

  void set_registry(const mistral::console::slash_command_registry* registry) {
    registry_ = registry;
  }

  void set_filter(const std::string& filter) {
    filter_ = filter;
    update_matched_commands();
    selected_index_ = 0;
  }

  void move_selection(int delta) {
    if (matched_commands_.empty()) return;
    selected_index_ = (selected_index_ + delta +
      (int)matched_commands_.size()) % (int)matched_commands_.size();
  }

  std::string selected_command_name() const {
    if (selected_index_ >= 0 &&
        selected_index_ < (int)matched_commands_.size()) {
      return matched_commands_[selected_index_];
    }
    return "";
  }

  int matched_count() const { return (int)matched_commands_.size(); }

  int popup_height() const {
    int rows = std::min(matched_count(), max_visible);
    if (rows == 0) rows = 1; // "No matching commands"
    return rows + 2; // +2 for top/bottom border
  }

  void render(cpptui::Buffer& buffer) override {
    using namespace cpptui;

    Color border_fg = fg_color.resolve(Theme::current().border);
    Color sel_bg = Theme::current().selection;
    Color sel_fg = Theme::current().input_fg;
    Color norm_bg = Theme::current().panel_bg;
    Color norm_fg = Theme::current().foreground;

    int w = width;
    int h = height;
    if (w < 2 || h < 2) return;

    int inner_w = w - 2;
    int inner_h = h - 2;

    // Top border
    {
      Cell c;
      c.fg_color = border_fg;
      c.content = "\xe2\x94\x8c"; // ┌
      buffer.set(x, y, c);
      c.content = "\xe2\x94\x90"; // ┐
      buffer.set(x + w - 1, y, c);
      c.content = "\xe2\x94\x80"; // ─
      for (int i = 1; i < w - 1; ++i) {
        buffer.set(x + i, y, c);
      }
    }

    // Bottom border
    {
      Cell c;
      c.fg_color = border_fg;
      c.content = "\xe2\x94\x94"; // └
      buffer.set(x, y + h - 1, c);
      c.content = "\xe2\x94\x98"; // ┘
      buffer.set(x + w - 1, y + h - 1, c);
      c.content = "\xe2\x94\x80"; // ─
      for (int i = 1; i < w - 1; ++i) {
        buffer.set(x + i, y + h - 1, c);
      }
    }

    // Side borders for all content rows
    for (int row = 0; row < inner_h; ++row) {
      Cell vc;
      vc.fg_color = border_fg;
      vc.content = "\xe2\x94\x82"; // │
      buffer.set(x, y + 1 + row, vc);
      buffer.set(x + w - 1, y + 1 + row, vc);
    }

    if (matched_commands_.empty()) {
      // "No matching commands"
      for (int i = 1; i < w - 1; ++i) {
        Cell c;
        c.content = " ";
        c.fg_color = norm_fg;
        c.bg_color = norm_bg;
        buffer.set(x + i, y + 1, c);
      }
      std::string msg = "No matching commands";
      auto chars = TextHelper::prepare_text_for_render(msg);
      int dx = 0;
      for (const auto& ci : chars) {
        if (1 + dx >= w - 1) break;
        Cell c;
        c.content = ci.content;
        c.fg_color = norm_fg;
        c.bg_color = norm_bg;
        buffer.set(x + 1 + dx, y + 1, c);
        if (ci.display_width == 2 && 1 + dx + 1 < w - 1) {
          Cell skip;
          skip.content = "";
          skip.bg_color = norm_bg;
          buffer.set(x + 1 + dx + 1, y + 1, skip);
        }
        dx += ci.display_width;
      }
      return;
    }

    // Determine scroll offset so selected row stays visible
    int scroll_offset = 0;
    if ((int)matched_commands_.size() > max_visible &&
        selected_index_ >= max_visible) {
      scroll_offset = selected_index_ - max_visible + 1;
    }

    int shown = std::min((int)matched_commands_.size() - scroll_offset,
                         max_visible);
    shown = std::min(shown, inner_h);

    for (int row = 0; row < shown; ++row) {
      int cmd_idx = scroll_offset + row;
      int abs_y = y + 1 + row;
      bool selected = (cmd_idx == selected_index_);
      Color bg = selected ? sel_bg : norm_bg;
      Color fg = selected ? sel_fg : norm_fg;

      // Fill row background
      for (int i = 1; i < w - 1; ++i) {
        Cell c;
        c.content = " ";
        c.fg_color = fg;
        c.bg_color = bg;
        buffer.set(x + i, abs_y, c);
      }

      // Column 1: /command_name
      std::string col1 = "/" + matched_commands_[cmd_idx];
      auto chars1 = TextHelper::prepare_text_for_render(col1);
      int dx = 0;
      for (const auto& ci : chars1) {
        if (1 + dx >= w - 1 || dx >= col1_width) break;
        Cell c;
        c.content = ci.content;
        c.fg_color = fg;
        c.bg_color = bg;
        buffer.set(x + 1 + dx, abs_y, c);
        if (ci.display_width == 2 && 1 + dx + 1 < w - 1 &&
            dx + 1 < col1_width) {
          Cell skip;
          skip.content = "";
          skip.bg_color = bg;
          buffer.set(x + 1 + dx + 1, abs_y, skip);
        }
        dx += ci.display_width;
      }

      // Column 2: description
      const auto* cmd = registry_->get_command(matched_commands_[cmd_idx]);
      std::string desc = cmd ? cmd->description() : "";
      int col2_start = 1 + col1_width;
      int col2_width = inner_w - col1_width;
      if (col2_width > 0 && !desc.empty()) {
        auto chars2 = TextHelper::prepare_text_for_render(desc);
        int total_w = 0;
        for (const auto& ci : chars2) total_w += ci.display_width;

        if (total_w > col2_width) {
          // Truncate with …
          int remaining_w = col2_width - 1;
          int dx2 = 0;
          for (const auto& ci : chars2) {
            if (remaining_w < ci.display_width) break;
            if (col2_start + dx2 >= w - 1) break;
            Cell c;
            c.content = ci.content;
            c.fg_color = fg;
            c.bg_color = bg;
            buffer.set(x + col2_start + dx2, abs_y, c);
            if (ci.display_width == 2 && col2_start + dx2 + 1 < w - 1) {
              Cell skip;
              skip.content = "";
              skip.bg_color = bg;
              buffer.set(x + col2_start + dx2 + 1, abs_y, skip);
            }
            dx2 += ci.display_width;
            remaining_w -= ci.display_width;
          }
          if (col2_start + dx2 < w - 1) {
            Cell c;
            c.content = "\xe2\x80\xa6"; // …
            c.fg_color = fg;
            c.bg_color = bg;
            buffer.set(x + col2_start + dx2, abs_y, c);
          }
        } else {
          int dx2 = 0;
          for (const auto& ci : chars2) {
            if (col2_start + dx2 >= w - 1) break;
            Cell c;
            c.content = ci.content;
            c.fg_color = fg;
            c.bg_color = bg;
            buffer.set(x + col2_start + dx2, abs_y, c);
            if (ci.display_width == 2 && col2_start + dx2 + 1 < w - 1) {
              Cell skip;
              skip.content = "";
              skip.bg_color = bg;
              buffer.set(x + col2_start + dx2 + 1, abs_y, skip);
            }
            dx2 += ci.display_width;
          }
        }
      }
    }

    // Fill any remaining rows with empty background
    for (int row = shown; row < inner_h; ++row) {
      int abs_y = y + 1 + row;
      for (int i = 1; i < w - 1; ++i) {
        Cell c;
        c.content = " ";
        c.bg_color = norm_bg;
        buffer.set(x + i, abs_y, c);
      }
    }
  }

private:
  void update_matched_commands() {
    matched_commands_.clear();
    if (!registry_) return;
    auto names = registry_->get_command_names();
    for (const auto& name : names) {
      if (filter_.empty() || starts_with_ci(name, filter_)) {
        matched_commands_.push_back(name);
      }
    }
  }

  static bool starts_with_ci(const std::string& str,
                             const std::string& prefix) {
    if (prefix.size() > str.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(str[i])) !=
          std::tolower(static_cast<unsigned char>(prefix[i]))) {
        return false;
      }
    }
    return true;
  }

  const mistral::console::slash_command_registry* registry_ = nullptr;
  std::string filter_;
  std::vector<std::string> matched_commands_;
  int selected_index_ = 0;
};

// ---------------------------------------------------------------------------
// ChatInput: multiline input area based on TextArea.
// Enter inserts a newline. Alt+Enter or Enter on an empty last line
// (double-Enter) submits the message.
//
// Approval mode state machine:
//   none        → normal chat input
//   single_key  → intercepts y/n/r/a/? before TextArea processes them
//   text_input  → normal editing for reject-with-message, Enter submits
// ---------------------------------------------------------------------------
class ChatInput : public cpptui::TextArea {
public:
  enum class approval_mode { none, single_key, text_input };

  std::function<void()> on_submit;
  std::function<void(char)> on_approval_key;
  std::function<void()> on_approval_text_submit;

  approval_mode approval_mode_ = approval_mode::none;

  // Slash command mode
  std::function<void(std::string, std::string)> on_slash_command;
  std::function<bool()> is_agent_busy;

  void set_slash_popup(SlashCommandPopup* popup) { popup_ = popup; }
  void set_slash_divider(Divider* divider) { slash_divider_ = divider; }
  bool slash_mode_active() const { return slash_mode_active_; }

  void cancel_slash_mode() {
    exit_slash_mode();
    set_text("");
  }

  void set_approval_mode(approval_mode mode) {
    approval_mode_ = mode;
    if (mode == approval_mode::none) {
      set_text("");
    } else if (mode == approval_mode::single_key) {
      set_text("");
    }
    // text_input mode: keep existing text, user is typing rejection message
  }

  bool on_event(const cpptui::Event& event) override {
    if (approval_mode_ == approval_mode::single_key) {
      // Intercept single-key approval responses before TextArea processes them.
      if (event.is_key_event() && event.is_printable() &&
          !event.ctrl && !event.alt && !event.shift) {
        char ch = static_cast<char>(event.key);
        if (ch == 'y' || ch == 'n' || ch == 'r' || ch == 'a' || ch == '?' ||
            ch == 'Y' || ch == 'N' || ch == 'R' || ch == 'A') {
          if (on_approval_key) on_approval_key(ch);
          return true;
        }
      }
      // Consume all other key events in single_key mode to prevent
      // TextArea from inserting characters.
      if (event.is_key_event()) {
        return true;
      }
      return TextArea::on_event(event);
    }

    if (approval_mode_ == approval_mode::text_input) {
      // In text_input mode, Enter submits the rejection message.
      if (event.is_key_event() && event.is_enter()) {
        if (!event.ctrl && !event.shift) {
          // Any Enter submits in text_input mode (no double-Enter needed).
          if (on_approval_text_submit) on_approval_text_submit();
          return true;
        }
      }
      // Let TextArea handle all other events (typing, backspace, etc.)
      return TextArea::on_event(event);
    }

    // Slash command mode: intercept navigation, tab, enter.
    if (approval_mode_ == approval_mode::none && slash_mode_active_) {
      if (event.is_key_event() && event.is_nav_up() &&
          !event.shift && !event.ctrl) {
        if (popup_) popup_->move_selection(-1);
        return true;
      }
      if (event.is_key_event() && event.is_nav_down() &&
          !event.shift && !event.ctrl) {
        if (popup_) popup_->move_selection(1);
        return true;
      }
      if (event.is_key_event() && event.is_tab()) {
        if (popup_) {
          std::string cmd = popup_->selected_command_name();
          if (!cmd.empty()) {
            set_text("/" + cmd + " ");
            // set_text resets cursor to 0; move it to end of line.
            cursor_x_ = (int)cpptui::prepare_text_for_render(
              lines_[cursor_y_]).size();
            ensure_cursor_visible();
          }
        }
        exit_slash_mode();
        return true;
      }
      if (event.is_key_event() && event.is_enter() &&
          !event.ctrl && !event.shift) {
        // Enter inserts the selected command into the input area
        // (like Tab completion), then exits slash mode. The user can
        // press Enter again through normal submit to execute.
        if (popup_) {
          std::string cmd = popup_->selected_command_name();
          if (!cmd.empty()) {
            set_text("/" + cmd + " ");
            // set_text resets cursor to 0; move it to end of line.
            cursor_x_ = (int)cpptui::prepare_text_for_render(
              lines_[cursor_y_]).size();
            ensure_cursor_visible();
          }
        }
        exit_slash_mode();
        return true;
      }
    }

    // Normal chat mode: existing Enter handling.
    if (event.is_key_event() && event.is_enter()) {
      // Slash command submission: when not in slash popup mode and
      // text starts with '/', single Enter executes the command
      // via on_slash_command (no double-Enter needed).
      if (!event.ctrl && !event.shift && !slash_mode_active_ &&
          on_slash_command) {
        std::string text = get_text();
        if (!text.empty() && text[0] == '/') {
          std::string after_slash = text.substr(1);
          size_t space_pos = after_slash.find(' ');
          std::string cmd = (space_pos == std::string::npos)
            ? after_slash
            : after_slash.substr(0, space_pos);
          std::string args = (space_pos == std::string::npos)
            ? ""
            : after_slash.substr(space_pos + 1);
          // Trim trailing whitespace from args.
          while (!args.empty() && args.back() == ' ') {
            args.pop_back();
          }
          set_text("");
          on_slash_command(cmd, args);
          return true;
        }
      }

      // Alt+Enter always submits.
      if (event.alt) {
        if (on_submit) on_submit();
        return true;
      }

      // Enter on empty last line with content above submits (double-Enter).
      if (!event.ctrl && !event.shift) {
        bool is_last_line = (cursor_y_ == (int)lines_.size() - 1);
        bool current_line_empty = lines_[cursor_y_].empty();
        bool has_content = false;
        for (const auto& l : lines_) {
          if (!l.empty()) { has_content = true; break; }
        }
        if (is_last_line && current_line_empty && has_content) {
          if (on_submit) on_submit();
          return true;
        }
      }
    }

    bool consumed = TextArea::on_event(event);

    // After TextArea processes, update slash mode state.
    if (approval_mode_ == approval_mode::none) {
      std::string text = get_text();
      if (slash_mode_active_) {
        if (text.empty() || text[0] != '/') {
          exit_slash_mode();
        } else {
          std::string after_slash = text.substr(1);
          size_t space_pos = after_slash.find(' ');
          if (space_pos != std::string::npos) {
            // User typed a space — command name is complete,
            // exit slash mode. User is now typing arguments.
            exit_slash_mode();
          } else if (popup_) {
            popup_->set_filter(after_slash);
            popup_->fixed_height = popup_->popup_height();
          }
        }
      } else if (text.size() == 1 && text[0] == '/' &&
                 event.is_key_event() && event.is_printable() &&
                 !event.ctrl && !event.alt && !event.shift) {
        if (!is_agent_busy || !is_agent_busy()) {
          enter_slash_mode();
        }
      }
    }

    return consumed;
  }

private:
  void enter_slash_mode() {
    slash_mode_active_ = true;
    if (popup_) {
      popup_->set_filter("");
      popup_->fixed_height = popup_->popup_height();
      popup_->visible = true;
    }
    if (slash_divider_) slash_divider_->visible = true;
  }

  void exit_slash_mode() {
    slash_mode_active_ = false;
    if (popup_) popup_->visible = false;
    if (slash_divider_) slash_divider_->visible = false;
  }

  bool slash_mode_active_ = false;
  SlashCommandPopup* popup_ = nullptr;
  Divider* slash_divider_ = nullptr;
};

#endif // MISTRAL_TUI_WIDGETS_H