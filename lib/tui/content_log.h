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
  //
  // Collapse: blocks can be collapsed to show a single summary line instead
  // of their full wrapped content. Auto-collapse happens per-block when the
  // block is "done" (i.e. a new block of a different type is created).
  // Manual collapse/expand is also supported via toggle_collapse().
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
    std::string tool_status;   // "" = none, "✓" or "✗" for tool_call blocks
  };

  // ---------------------------------------------------------------------------
  // ContentLog: block-based content widget replacing Paragraph in content_area.
  //
  // Each content fragment is accumulated into a block (merging with the last
  // block if same kind, or creating a new one). On flush(), only dirty blocks
  // are re-wrapped via Paragraph::wrap_text_static — O(dirty_block_size)
  // instead of the current O(total_text) re-wrap of the entire content.
  //
  // Collapse/Expand: blocks can be collapsed to show a summary line. Thinking
  // and tool_result blocks auto-collapse when the next block is created.
  // Text (final answer) blocks auto-collapse when complete_turn() is called
  // (i.e. when the next turn starts). Manual toggle is supported via
  // toggle_collapse().
  //
  // Render iterates all blocks and writes their cached_lines (or summary if
  // collapsed) to the buffer. The parent ScrollableVertical adjusts this
  // widget's y position by scroll_offset and handles clipping, so no
  // scroll_offset access is needed.
  // ---------------------------------------------------------------------------
  class ContentLog : public cpptui::Widget
  {
  public:
    using content_type = mistral::agent::content_type;

    ContentLog() { focusable = false; }

    // O(1) accumulation of a text fragment into the last block (if same kind)
    // or a new block. Does NOT re-wrap — just accumulates text.
    //
    // When force_new is true, always creates a new block even if the last
    // block has the same kind. This is useful for loading historical
    // conversations where each tool call/result should be a separate,
    // individually collapsible block.
    //
    // When a new block is created, the previous block is auto-collapsed if
    // it is an auto-collapsible type (thinking, tool_result). When a
    // tool_result block is created, the first pending tool_call block's
    // status is updated to "✓".
    void append_fragment(const std::string& fragment, content_type type,
                         bool force_new = false)
    {
      block_style style = map_content_type(type);
      bool create_new = force_new || blocks_.empty() ||
                        blocks_.back().kind != style.kind;

      if (create_new) {
        // Auto-collapse the previous block if it's auto-collapsible.
        if (!blocks_.empty() && is_auto_collapsible(blocks_.back().kind)) {
          blocks_.back().collapsed = true;
        }

        ContentBlock block;
        block.kind = style.kind;
        block.text = fragment;
        block.color = style.color;
        block.bold = style.bold;
        block.dirty = true;
        blocks_.push_back(std::move(block));

        // Update the first pending tool_call's status when a tool_result
        // block is created.
        if (style.kind == BlockKind::tool_result) {
          update_first_pending_tool_call_status(true);
        }
      } else {
        blocks_.back().text += fragment;
        blocks_.back().dirty = true;
      }
    }

    // Re-wraps only dirty blocks via Paragraph::wrap_text_static and
    // recalculates total_lines_ accounting for collapsed blocks.
    // Sets fixed_height = total_lines_ so ScrollableVertical computes
    // content height correctly.
    void flush()
    {
      int w = width > 0 ? width : 80;
      for (auto& block : blocks_) {
        if (!block.dirty) continue;
        block.cached_lines = cpptui::Paragraph::wrap_text_static(
          block.text, w, 0, 0, true);
        block.cached_width = w;
        block.dirty = false;
      }
      recalc_total_lines();
    }

    // Renders all blocks to the buffer. Collapsed blocks render their
    // summary line; expanded blocks render their cached_lines.
    // Non-virtualized: iterates all blocks, renders all lines. The parent
    // ScrollableVertical's clip rect handles out-of-bounds suppression.
    void render(cpptui::Buffer& buffer) override
    {
      if (width <= 0) return;
      cpptui::Color bg = cpptui::Theme::current().background;
      int current_y = y;
      for (const auto& block : blocks_) {
        if (block.collapsed) {
          std::string line = generate_summary(block);
          render_line(buffer, line, block, bg, current_y);
          current_y++;
        } else {
          for (const auto& line : block.cached_lines) {
            render_line(buffer, line, block, bg, current_y);
            current_y++;
          }
        }
      }
    }

    // --- Collapse/Expand ---

    // Collapse all text blocks (final answers) from previous turns.
    // Called when a new turn starts (e.g. when a new user message arrives).
    void complete_turn()
    {
      for (auto& block : blocks_) {
        if (block.kind == BlockKind::text) {
          block.collapsed = true;
        }
      }
      recalc_total_lines();
    }

    // Toggle collapse/expand of the block at the given index.
    // Only collapsible block types (thinking, tool_result, text) can be
    // toggled. Returns true if the block was toggled.
    bool toggle_collapse(int block_index)
    {
      if (block_index < 0 || block_index >= (int)blocks_.size()) return false;
      auto& block = blocks_[block_index];
      if (!is_collapsible(block.kind)) return false;
      block.collapsed = !block.collapsed;
      recalc_total_lines();
      return true;
    }

    // Find the block index that contains the given line offset.
    // Returns -1 if the line is outside all blocks.
    int block_at_line(int line) const
    {
      int current = 0;
      for (int i = 0; i < (int)blocks_.size(); ++i) {
        int block_lines = blocks_[i].collapsed
          ? 1 : (int)blocks_[i].cached_lines.size();
        if (line >= current && line < current + block_lines) {
          return i;
        }
        current += block_lines;
      }
      return -1;
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

    // Returns true if the block kind can be manually collapsed/expanded.
    static bool is_collapsible(BlockKind kind)
    {
      return kind == BlockKind::thinking ||
             kind == BlockKind::tool_result ||
             kind == BlockKind::text;
    }

    // Returns true if the block kind auto-collapses when the next block
    // is created (i.e. when the block is "done" streaming).
    static bool is_auto_collapsible(BlockKind kind)
    {
      return kind == BlockKind::thinking ||
             kind == BlockKind::tool_result;
    }

    // Render a single line of a block to the buffer at the given y position.
    void render_line(cpptui::Buffer& buffer, const std::string& line,
                     const ContentBlock& block,
                     const cpptui::Color& bg, int row_y)
    {
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
        buffer.set(x + dx, row_y, c);
        if (ci.display_width == 2 && dx + 1 < width) {
          cpptui::Cell skip;
          skip.content = "";
          skip.bg_color = bg;
          buffer.set(x + dx + 1, row_y, skip);
        }
        dx += ci.display_width;
      }
      // Fill remaining width with background
      for (int i = dx; i < width; ++i) {
        cpptui::Cell c;
        c.content = " ";
        c.fg_color = fg;
        c.bg_color = bg;
        buffer.set(x + i, row_y, c);
      }
    }

    // Generate a one-line summary for a collapsed block.
    std::string generate_summary(const ContentBlock& block) const
    {
      int line_count = (int)block.cached_lines.size();
      switch (block.kind) {
        case BlockKind::thinking:
          return "\xf0\x9f\x92\xad Thinking... (" +
                 std::to_string(line_count) + " lines)";
        case BlockKind::tool_result: {
          // Find first non-empty line for the summary
          std::string first_line;
          for (const auto& line : block.cached_lines) {
            if (!line.empty()) {
              first_line = line;
              break;
            }
          }
          if (first_line.size() > 60) {
            first_line = first_line.substr(0, 60) + "...";
          }
          return first_line + " (" + std::to_string(line_count) + " lines)";
        }
        case BlockKind::text: {
          std::string first_line;
          for (const auto& line : block.cached_lines) {
            if (!line.empty()) {
              first_line = line;
              break;
            }
          }
          if (first_line.size() > 60) {
            first_line = first_line.substr(0, 60) + "...";
          }
          return first_line + " (" + std::to_string(line_count) + " lines)";
        }
        default:
          return block.text;
      }
    }

    // Update the first tool_call block that doesn't have a status yet.
    void update_first_pending_tool_call_status(bool success)
    {
      for (auto& block : blocks_) {
        if (block.kind == BlockKind::tool_call &&
            block.tool_status.empty()) {
          block.tool_status = success ? "\xe2\x9c\x93" : "\xe2\x9c\x97";
          block.text += " " + block.tool_status;
          block.dirty = true;
          break;
        }
      }
    }

    // Recalculate total_lines_ from scratch, accounting for collapsed blocks.
    void recalc_total_lines()
    {
      total_lines_ = 0;
      for (const auto& block : blocks_) {
        total_lines_ += block.collapsed
          ? 1 : (int)block.cached_lines.size();
      }
      fixed_height = total_lines_;
    }

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