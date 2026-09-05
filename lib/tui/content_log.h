#ifndef MISTRAL_TUI_CONTENT_LOG_H
#define MISTRAL_TUI_CONTENT_LOG_H

#include "tui/tui_render_sink.h"

#include <cpptui.hpp>

#include <string>
#include <utility>
#include <vector>

namespace mistral::tui
{
  // ---------------------------------------------------------------------------
  // BlockKind: display category for a content block.
  // Maps 1:1 to content_type from tui_render_sink.h, with "text" replacing
  // "normal" for clarity in the block-based model.
  // ---------------------------------------------------------------------------
  enum class BlockKind {
    user,        // "> user message"
    thinking,    // collapsible, dimmed
    text,        // normal AI response text
    tool_call,   // "[tool] read(path=...)" — header, always visible
    tool_result, // "→ result..." — collapsible, can be very large
    error,       // "[error] ..."
    warning,     // "[warning] ..."
    system,      // approval prompts, etc.
  };

  // ---------------------------------------------------------------------------
  // ContentBlock: a single immutable (after flush) chunk of content.
  // Wrapping is cached per-block so only the modified block needs re-wrapping
  // on flush — O(last_block_size) instead of O(total_text).
  // ---------------------------------------------------------------------------
  struct ContentBlock {
    BlockKind kind = BlockKind::text;
    std::string text;
    cpptui::Color color;       // Resolved from theme at append time
    bool bold = false;
    bool collapsed = false;
    std::string summary;       // One-line summary when collapsed
    // Cached wrapping (invalidated on width change or collapse toggle)
    mutable std::vector<std::string> cached_lines;
    mutable int cached_width = 0;
    bool dirty = false;        // True if text changed since last flush
  };

  // ---------------------------------------------------------------------------
  // ContentLog: block-based content widget replacing Paragraph in content_area.
  //
  // Each content fragment is accumulated into a block (merging with the last
  // block if same kind, or creating a new one). On flush(), only dirty blocks
  // are re-wrapped via Paragraph::wrap_text_static — O(dirty_block_size)
  // instead of the current O(total_text) re-wrap of the entire content.
  //
  // Render iterates all blocks and writes their cached_lines to the buffer.
  // The parent ScrollableVertical adjusts this widget's y position by
  // scroll_offset and handles clipping, so no scroll_offset access is needed.
  // ---------------------------------------------------------------------------
  class ContentLog : public cpptui::Widget
  {
  public:
    using content_type = mistral::agent::content_type;

    ContentLog() { focusable = false; }

    // O(1) accumulation of a text fragment into the last block (if same kind)
    // or a new block. Does NOT re-wrap — just accumulates text.
    void append_fragment(const std::string& fragment, content_type type)
    {
      block_style style = map_content_type(type);
      if (!blocks_.empty() && blocks_.back().kind == style.kind) {
        blocks_.back().text += fragment;
        blocks_.back().dirty = true;
      } else {
        ContentBlock block;
        block.kind = style.kind;
        block.text = fragment;
        block.color = style.color;
        block.bold = style.bold;
        block.dirty = true;
        blocks_.push_back(std::move(block));
      }
    }

    // Re-wraps only dirty blocks via Paragraph::wrap_text_static and updates
    // total_lines_ by the delta. Sets fixed_height = total_lines_ so
    // ScrollableVertical computes content height correctly.
    void flush()
    {
      int w = width > 0 ? width : 80;
      for (auto& block : blocks_) {
        if (!block.dirty) continue;
        int old_lines = (int)block.cached_lines.size();
        block.cached_lines = cpptui::Paragraph::wrap_text_static(
          block.text, w, 0, 0, true);
        block.cached_width = w;
        block.dirty = false;
        total_lines_ += (int)block.cached_lines.size() - old_lines;
      }
      fixed_height = total_lines_;
    }

    // Renders all blocks and their cached lines to the buffer.
    // Non-virtualized: iterates all blocks, renders all lines. The parent
    // ScrollableVertical's clip rect handles out-of-bounds suppression.
    void render(cpptui::Buffer& buffer) override
    {
      if (width <= 0) return;
      cpptui::Color bg = cpptui::Theme::current().background;
      int current_y = y;
      for (const auto& block : blocks_) {
        for (const auto& line : block.cached_lines) {
          cpptui::Color fg = block.color.is_default
            ? cpptui::Theme::current().foreground
            : block.color;
          auto chars = cpptui::TextHelper::prepare_text_for_render(line);
          int dx = 0;
          for (const auto& ci : chars) {
            if (dx >= width) break;
            cpptui::Cell c;
            c.content = ci.content;
            c.fg_color = fg;
            c.bg_color = bg;
            c.bold = block.bold;
            buffer.set(x + dx, current_y, c);
            if (ci.display_width == 2 && dx + 1 < width) {
              cpptui::Cell skip;
              skip.content = "";
              skip.bg_color = bg;
              buffer.set(x + dx + 1, current_y, skip);
            }
            dx += ci.display_width;
          }
          // Fill remaining width with background
          for (int i = dx; i < width; ++i) {
            cpptui::Cell c;
            c.content = " ";
            c.fg_color = fg;
            c.bg_color = bg;
            buffer.set(x + i, current_y, c);
          }
          current_y++;
        }
      }
    }

    // --- Test accessors ---

    int block_count() const { return (int)blocks_.size(); }
    int total_lines() const { return total_lines_; }
    const ContentBlock& block_at(int index) const { return blocks_.at(index); }

    void clear()
    {
      blocks_.clear();
      total_lines_ = 0;
      fixed_height = 0;
    }

  private:
    struct block_style {
      BlockKind kind;
      cpptui::Color color;
      bool bold;
    };

    // Maps content_type to BlockKind + theme color + bold.
    // Same color mapping as content_area::append_fragment.
    static block_style map_content_type(content_type type)
    {
      using ct = content_type;
      switch (type) {
        case ct::normal:
          return {BlockKind::text, cpptui::Color(), false};
        case ct::thinking:
          return {BlockKind::thinking,
                  cpptui::Theme::current().input_placeholder, false};
        case ct::tool_call:
          return {BlockKind::tool_call,
                  cpptui::Theme::current().primary, true};
        case ct::tool_result:
          return {BlockKind::tool_result,
                  cpptui::Theme::current().primary, false};
        case ct::error:
          return {BlockKind::error,
                  cpptui::Theme::current().error, true};
        case ct::warning:
          return {BlockKind::warning,
                  cpptui::Theme::current().warning, false};
        case ct::user:
          return {BlockKind::user,
                  cpptui::Theme::current().success, true};
        case ct::system:
          return {BlockKind::system,
                  cpptui::Theme::current().warning, false};
      }
      return {BlockKind::text, cpptui::Color(), false};
    }

    std::vector<ContentBlock> blocks_;
    int total_lines_ = 0;
  };

} // namespace mistral::tui

#endif // MISTRAL_TUI_CONTENT_LOG_H