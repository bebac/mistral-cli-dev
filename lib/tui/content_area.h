#ifndef MISTRAL_TUI_CONTENT_AREA_H
#define MISTRAL_TUI_CONTENT_AREA_H

#include "tui/tui_render_sink.h"
#include "tui/content_log.h"

#include <cpptui.hpp>

#include <functional>
#include <memory>
#include <string>

namespace mistral::tui
{
  // ---------------------------------------------------------------------------
  // resize_aware_scroll: ScrollableVertical that detects terminal width
  // changes.
  //
  // cpptui never dispatches Resize events to widgets — its App loop only
  // relayouts the tree and re-renders. However, App::run() calls layout()
  // on every redraw (including the first redraw after a resize debounce),
  // and Container::layout() is virtual, so overriding it provides an exact
  // hook to re-wrap content before it is rendered at the new size.
  // ---------------------------------------------------------------------------
  class resize_aware_scroll : public cpptui::ScrollableVertical
  {
  public:
    std::function<void()> on_layout;

    void layout() override
    {
      cpptui::ScrollableVertical::layout();
      if (on_layout) on_layout();
    }
  };

  // content_area: manages the scrollable content region of the TUI.
  //
  // Owns the ContentLog widget (block-based content with cached
  // per-block wrapping and virtualized rendering) and the
  // ScrollableVertical container. Provides:
  //   - append_fragment(text, type)  — O(1) block accumulation
  //   - flush()                      — O(dirty_blocks) re-wrap + scroll update
  //   - append(text, type)           — convenience: append + flush
  //   - scroll_page_up/down()        — page scrolling with clamping
  //   - scroll_line_up/down()        — line scrolling with clamping
  //   - scroll_to_bottom()           — jump to bottom (used by approval timer)
  //   - was_at_bottom()              — query last flush scroll position
  //
  // The content_type to BlockKind + color mapping is encapsulated in
  // ContentLog (via map_content_type). This layer strips inter-block
  // spacing newlines that the old Paragraph-based code relied on,
  // since ContentLog handles spacing at render time via
  // needs_separator_before().
  class content_area
  {
  public:
    using content_type = mistral::agent::content_type;

    // Creates the ContentLog and ScrollableVertical widgets, sets
    // initial welcome text, and performs the initial wrap. Returns
    // the ScrollableVertical for insertion into the layout tree.
    std::shared_ptr<cpptui::Widget> build()
    {
      content_ = std::make_shared<ContentLog>();

      content_->append_fragment(
        "mistral-tui prototype\n"
        "Type your message. Alt+Enter or double-Enter to send. "
        "ESC to cancel. Ctrl+C to quit.",
        content_type::normal);
      content_->flush();

      auto scroll = std::make_shared<resize_aware_scroll>();
      scroll->add(content_);
      scroll->focusable = false;
      // Re-wrap content when the terminal is resized (see
      // resize_aware_scroll).
      scroll->on_layout = [this]() { check_resize(); };
      scroll_ = scroll;

      return scroll_;
    }

    // O(1) accumulation of a text fragment into the ContentLog.
    //
    // For complete-message types (user, error, warning, system),
    // strips leading and trailing newlines — these are inter-block
    // spacing that ContentLog handles at render time via
    // needs_separator_before().
    //
    // For streaming types (normal, thinking, tool_call, tool_result),
    // fragments are passed as-is to ContentLog. Leading newlines are
    // NOT stripped because they may be part of the content (e.g. blank
    // lines between paragraphs, newlines in ASCII art). ContentLog
    // handles leading newline stripping at block-type boundaries for
    // text blocks, and strip_leading_ws for tool_call blocks.
    //
    // Empty fragments are skipped.
    void append_fragment(const std::string& fragment, content_type type)
    {
      std::string text = fragment;

      // For complete-message types, strip leading and trailing
      // newlines — these are inter-block spacing.
      if (type == content_type::user || type == content_type::error ||
          type == content_type::warning || type == content_type::system) {
        // Strip leading newlines
        size_t start = 0;
        while (start < text.size() && text[start] == '\n') ++start;
        if (start > 0) text = text.substr(start);
        // Strip trailing newlines
        size_t end = text.size();
        while (end > 0 &&
               (text[end - 1] == '\n' || text[end - 1] == '\r')) {
          --end;
        }
        if (end < text.size()) text = text.substr(0, end);
      }

      // Skip empty fragments.
      if (text.empty()) return;

      content_->append_fragment(text, type);
    }

    // Re-wraps only dirty blocks and updates scroll position.
    // If the user was at the bottom before the update, auto-scrolls
    // to follow new content. Otherwise preserves their scroll position.
    void flush()
    {
      int visible_h = scroll_->height > 0 ? scroll_->height : 1;
      int ms = scroll_->content_height - visible_h;
      bool was_at_bottom = (scroll_->scroll_offset >= (ms < 0 ? 0 : ms));
      at_bottom_ = was_at_bottom;

      content_->flush();
      last_flushed_width_ = content_->width;

      if (was_at_bottom) {
        int max_scroll = content_->total_lines() - visible_h;
        if (max_scroll < 0) max_scroll = 0;
        scroll_->scroll_offset = max_scroll;
      } else {
        // Re-wrapping changes the line count; keep the scroll offset
        // within the new content range.
        clamp_scroll();
      }
    }

    // Convenience: append_fragment + flush.
    void append(const std::string& text, content_type type)
    {
      append_fragment(text, type);
      flush();
    }

    // Finalize the current turn: auto-collapse the last thinking block,
    // clean up trailing text, and update scroll position. Called when
    // the render_sink's completed() callback fires.
    void complete_turn()
    {
      content_->complete_turn();
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

    // Detects terminal width changes and re-wraps the content.
    //
    // cpptui does not dispatch Resize events to widgets — it only
    // relayouts the tree and re-renders. Called from the
    // resize_aware_scroll layout hook: compares the ContentLog's
    // current width (updated by layout) against the width used at the
    // last flush() and re-wraps all blocks when they differ.
    void check_resize()
    {
      if (!content_) return;
      if (content_->width != last_flushed_width_) {
        flush();
      }
    }

    std::shared_ptr<ContentLog> content_;
    std::shared_ptr<cpptui::ScrollableVertical> scroll_;
    bool at_bottom_ = true;
    // Width of the ContentLog at the last flush(); used by check_resize()
    // to detect terminal resizes.
    int last_flushed_width_ = 0;
  };

} // namespace mistral::tui

#endif // MISTRAL_TUI_CONTENT_AREA_H