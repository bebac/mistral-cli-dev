#ifndef MISTRAL_TUI_CONTENT_AREA_H
#define MISTRAL_TUI_CONTENT_AREA_H

#include "tui/tui_render_sink.h"

#include <cpptui.hpp>

#include <memory>
#include <string>

namespace mistral::tui
{
  // content_area: manages the scrollable content region of the TUI.
  //
  // Owns the Paragraph widget (with word-wrap and styled text), the
  // ScrollableVertical container, and the StyledText accumulator.
  // Provides:
  //   - append_fragment(text, type)  — O(1) styled text accumulation
  //   - flush()                      — O(n) re-wrap + scroll update
  //   - append(text, type)           — convenience: append + flush
  //   - scroll_page_up/down()        — page scrolling with clamping
  //   - scroll_line_up/down()        — line scrolling with clamping
  //   - scroll_to_bottom()           — jump to bottom (used by approval timer)
  //   - was_at_bottom()              — query last flush scroll position
  //
  // The content_type to Theme color mapping is encapsulated here so
  // future rendering enhancements (markdown, diff blocks, etc.) have a
  // single home.
  class content_area
  {
  public:
    using content_type = mistral::agent::content_type;

    // Creates the Paragraph and ScrollableVertical widgets, applies
    // theme colors, sets initial welcome text, and performs the
    // initial wrap. Returns the ScrollableVertical for insertion into
    // the layout tree.
    std::shared_ptr<cpptui::Widget> build()
    {
      using namespace cpptui;

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

      return scroll_;
    }

    // O(1) accumulation of a styled text fragment.
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

    // O(n) re-wrap of the full content and scroll position update.
    // If the user was at the bottom before the update, auto-scrolls
    // to follow new content. Otherwise preserves their scroll position.
    void flush()
    {
      int visible_h = scroll_->height > 0 ? scroll_->height : 1;
      int ms = scroll_->content_height - visible_h;
      bool was_at_bottom = (scroll_->scroll_offset >= (ms < 0 ? 0 : ms));
      at_bottom_ = was_at_bottom;

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

    // Convenience: append_fragment + flush.
    void append(const std::string& text, content_type type)
    {
      append_fragment(text, type);
      flush();
    }

    // Returns true if the last flush() found the user at the bottom.
    bool was_at_bottom() const { return at_bottom_; }

    // Sets scroll to the maximum offset (bottom of content).
    void scroll_to_bottom()
    {
      scroll_->scroll_offset = max_scroll_offset();
    }

    void scroll_page_up()
    {
      int visible_h = scroll_->height > 0 ? scroll_->height : 1;
      scroll_->scroll_offset -= visible_h;
      clamp_scroll();
    }

    void scroll_page_down()
    {
      int visible_h = scroll_->height > 0 ? scroll_->height : 1;
      scroll_->scroll_offset += visible_h;
      clamp_scroll();
    }

    void scroll_line_up()
    {
      scroll_->scroll_offset -= 1;
      clamp_scroll();
    }

    void scroll_line_down()
    {
      scroll_->scroll_offset += 1;
      clamp_scroll();
    }

  private:
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

    std::shared_ptr<cpptui::Paragraph> content_;
    std::shared_ptr<cpptui::ScrollableVertical> scroll_;
    cpptui::StyledText content_styled_;
    bool at_bottom_ = true;
  };

} // namespace mistral::tui

#endif // MISTRAL_TUI_CONTENT_AREA_H