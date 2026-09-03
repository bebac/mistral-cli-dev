#ifndef MISTRAL_TUI_WIDGETS_H
#define MISTRAL_TUI_WIDGETS_H

#include "cpptui.hpp"

#include <functional>
#include <string>

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

    // Normal chat mode: existing Enter handling.
    if (event.is_key_event() && event.is_enter()) {
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

    return TextArea::on_event(event);
  }
};

#endif // MISTRAL_TUI_WIDGETS_H