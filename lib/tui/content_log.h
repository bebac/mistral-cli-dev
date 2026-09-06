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
  //
  // tool_call and tool_result from content_type are merged into a single
  // "tool" block. The content_type enum in tui_render_sink.h stays as-is;
  // the merging happens inside ContentLog::append_fragment.
  // ---------------------------------------------------------------------------
  enum class BlockKind {
    user,                // "> user message"
    thinking,            // collapsible, dimmed, indented
    text,                // normal AI response text (always expanded)
    tool,                // merged tool_call + tool_result, collapsible
    error,               // "[error] ..."
    warning,             // "[warning] ..."
    system,              // approval prompts, etc.
    turn_separator,      // "---[N]---" full-width separator
  };

  // ---------------------------------------------------------------------------
  // ContentBlock: a single chunk of content.
  //
  // For tool blocks, `text` stores the call text (e.g. "[tool] name(args)")
  // and `result_text` stores the result text (e.g. "→ result...").
  // Both are wrapped independently in flush().
  //
  // Collapse: thinking and tool blocks can be collapsed to show a single
  // summary line with a ▶ indicator. Expanded blocks show a ▼ indicator.
  // Auto-collapse happens when the next block of a different type is created.
  // Manual toggle is supported via toggle_collapse().
  // ---------------------------------------------------------------------------
  struct ContentBlock {
    BlockKind kind = BlockKind::text;
    std::string text;
    std::string result_text;       // result text for tool blocks
    cpptui::Color color;           // resolved from theme at append time
    bool bold = false;
    bool collapsed = false;
    // Cached wrapping (invalidated on width change or content change)
    mutable std::vector<std::string> cached_lines;
    mutable std::vector<std::string> result_cached_lines; // for tool result
    mutable int cached_width = 0;
    mutable int result_cached_width = 0;
    bool dirty = false;
    bool result_dirty = false;
    std::string tool_status;       // "" = none, "✓" or "✗" for tool blocks
  };

  // ---------------------------------------------------------------------------
  // ContentLog: block-based content widget replacing Paragraph in content_area.
  //
  // Each content fragment is accumulated into a block (merging with the last
  // block if same kind, or creating a new one). On flush(), only dirty blocks
  // are re-wrapped via Paragraph::wrap_text_static — O(dirty_block_size)
  // instead of the current O(total_text) re-wrap of the entire content.
  //
  // Tool blocks: tool_call fragments create a new tool block; tool_result
  // fragments append to the last tool block's result_text. Collapsed tool
  // blocks show "[tool] name(truncated_args) ✓". Expanded tool blocks show
  // the full call text followed by indented result lines.
  //
  // Collapse/Expand: thinking and tool blocks auto-collapse when the next
  // block is created. Text blocks are always expanded. Manual toggle is
  // supported via toggle_collapse(). Collapsed blocks show ▶ prefix;
  // expanded blocks show ▼ prefix. Both are indented 2 spaces.
  //
  // Turn separators and final answer markers are special non-collapsible
  // blocks added via append_turn_separator() and append_final_answer_marker().
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

    // UTF-8 encodings for collapse/expand indicators.
    // ▶ (U+25B6) = triangle right (collapsed)
    // ▼ (U+25BC) = triangle down (expanded)
    // Both are display_width=1 in cpptui's char_display_width.
    static constexpr const char* COLLAPSED_IND = "\xe2\x96\xb6"; // ▶
    static constexpr const char* EXPANDED_IND  = "\xe2\x96\xbc"; // ▼
    // ✓ (U+2713) = check mark, ✗ (U+2717) = ballot X
    static constexpr const char* STATUS_OK    = "\xe2\x9c\x93"; // ✓
    static constexpr const char* STATUS_FAIL  = "\xe2\x9c\x97"; // ✗

    // Indent for collapsible blocks: 2 spaces.
    static constexpr int COLLAPSE_INDENT = 2;
    // Prefix width: indent (2) + indicator (1) + space (1) = 4.
    static constexpr int PREFIX_WIDTH = COLLAPSE_INDENT + 1 + 1;

    ContentLog() { focusable = false; }

    // O(1) accumulation of a text fragment into the last block (if same kind)
    // or a new block. Does NOT re-wrap — just accumulates text.
    //
    // tool_call: always creates a new tool block. Leading whitespace is
    //   stripped from the fragment.
    // tool_result: appends to the last tool block's result_text and sets
    //   its status to ✓. If no pending tool block exists, creates one.
    // Other types: merge with last block if same kind, else create new.
    //
    // When force_new is true, always creates a new block even if the last
    // block has the same kind (except for tool_result, which always appends
    // to the last tool block).
    //
    // When a new block is created, the previous block is auto-collapsed if
    // it is an auto-collapsible type (thinking, tool).
    void append_fragment(const std::string& fragment, content_type type,
                         bool force_new = false)
    {
      // --- Tool call: always create a new tool block ---
      // Tool blocks are always created collapsed — the tool output is
      // not useful to the human user. They can still be manually
      // expanded via toggle_collapse() if needed.
      if (type == content_type::tool_call) {
        cleanup_last_text_block();
        auto_collapse_previous();
        ContentBlock block;
        block.kind = BlockKind::tool;
        block.color = cpptui::Theme::current().primary;
        block.bold = true;
        block.text = strip_leading_ws(fragment);
        block.collapsed = true;
        block.dirty = true;
        blocks_.push_back(std::move(block));
        return;
      }

      // --- Tool result: append to a tool block ---
      if (type == content_type::tool_result) {
        // If the last block is a tool block, append to it (streaming
        // continuation of the same result).
        if (!blocks_.empty() && blocks_.back().kind == BlockKind::tool) {
          blocks_.back().result_text += fragment;
          blocks_.back().result_dirty = true;
          if (blocks_.back().tool_status.empty()) {
            blocks_.back().tool_status = STATUS_OK;
          }
          return;
        }
        // Last block is not a tool block — search backwards for a pending
        // tool block (one with empty result_text). This handles the case
        // where multiple tool_calls were emitted before any results.
        for (int i = (int)blocks_.size() - 1; i >= 0; --i) {
          if (blocks_[i].kind == BlockKind::tool &&
              blocks_[i].result_text.empty()) {
            blocks_[i].result_text += fragment;
            blocks_[i].result_dirty = true;
            if (blocks_[i].tool_status.empty()) {
              blocks_[i].tool_status = STATUS_OK;
            }
            return;
          }
        }
        // Edge case: no pending tool block — create one with only result.
        auto_collapse_previous();
        ContentBlock block;
        block.kind = BlockKind::tool;
        block.color = cpptui::Theme::current().primary;
        block.bold = true;
        block.result_text = fragment;
        block.result_dirty = true;
        block.tool_status = STATUS_OK;
        blocks_.push_back(std::move(block));
        return;
      }

      // --- Other types: standard merge-or-create logic ---
      block_style style = map_content_type(type);
      bool create_new = force_new || blocks_.empty() ||
                        blocks_.back().kind != style.kind;

      if (create_new) {
        auto_collapse_previous();
        ContentBlock block;
        block.kind = style.kind;
        block.text = fragment;
        if (style.kind == BlockKind::text) {
          block.text = strip_leading_newlines(block.text);
        }
        block.color = style.color;
        block.bold = style.bold;
        block.dirty = true;
        blocks_.push_back(std::move(block));
      } else {
        blocks_.back().text += fragment;
        if (blocks_.back().kind == BlockKind::text) {
          blocks_.back().text = strip_leading_newlines(blocks_.back().text);
        }
        blocks_.back().dirty = true;
      }
    }

    // Add a turn separator block: "---[N]" filled with '-' to full width.
    void append_turn_separator(int turn_number)
    {
      cleanup_last_text_block();
      ContentBlock block;
      block.kind = BlockKind::turn_separator;
      block.text = "---[" + std::to_string(turn_number) + "]";
      block.color = cpptui::Theme::current().input_placeholder;
      blocks_.push_back(std::move(block));
      recalc_total_lines();
    }



    // Re-wraps only dirty blocks via Paragraph::wrap_text_static and
    // recalculates total_lines_ accounting for collapsed blocks.
    // Sets fixed_height = total_lines_ so ScrollableVertical computes
    // content height correctly.
    //
    // Collapsible blocks (thinking, tool) are wrapped at width - PREFIX_WIDTH
    // to account for the indent + indicator + space prefix. Tool result
    // text is also wrapped at width - PREFIX_WIDTH (rendered with the same
    // indent). Non-collapsible blocks are wrapped at full width.
    void flush()
    {
      int w = width > 0 ? width : 80;
      for (auto& block : blocks_) {
        int wrap_w = is_collapsible(block.kind) ? w - PREFIX_WIDTH : w;
        if (wrap_w < 1) wrap_w = 1;

        if (block.dirty) {
          block.cached_lines = cpptui::Paragraph::wrap_text_static(
            block.text, wrap_w, 0, 0, true);
          block.cached_width = wrap_w;
          block.dirty = false;
        }
        if (block.result_dirty) {
          block.result_cached_lines = cpptui::Paragraph::wrap_text_static(
            block.result_text, wrap_w, 0, 0, true);
          block.result_cached_width = wrap_w;
          block.result_dirty = false;
        }
      }
      recalc_total_lines();
    }

    // Renders all blocks to the buffer.
    //
    // Collapsible blocks: render "  ▶ " + summary (collapsed) or
    // "  ▼ " + first line + "    " + remaining lines (expanded).
    // Tool blocks expanded: call lines then indented result lines.
    // Turn separators: text filled with '-' to full width.
    // Final answer markers: text as-is.
    // Non-collapsible blocks: cached_lines as-is.
    void render(cpptui::Buffer& buffer) override
    {
      if (width <= 0) return;
      cpptui::Color bg = cpptui::Theme::current().background;
      int current_y = y;
      for (int i = 0; i < (int)blocks_.size(); ++i) {
        const auto& block = blocks_[i];
        if (i > 0 && needs_separator_before(blocks_[i-1].kind, block.kind)) {
          render_blank_line(buffer, bg, current_y);
          current_y++;
        }
        if (block.kind == BlockKind::turn_separator) {
          std::string line = block.text;
          while ((int)line.size() < width) line += '-';
          render_line(buffer, line, block, bg, current_y);
          current_y++;
        } else if (is_collapsible(block.kind)) {
          if (block.collapsed) {
            std::string line = indent_str() + COLLAPSED_IND + " " +
                               generate_summary(block);
            render_line(buffer, line, block, bg, current_y);
            current_y++;
          } else {
            const std::string first_prefix = indent_str() +
              std::string(EXPANDED_IND) + " ";
            const std::string cont_prefix = std::string(PREFIX_WIDTH, ' ');
            bool first = true;
            for (const auto& cl : block.cached_lines) {
              std::string line = (first ? first_prefix : cont_prefix) + cl;
              render_line(buffer, line, block, bg, current_y);
              current_y++;
              first = false;
            }
            // Tool blocks: render indented result lines
            if (block.kind == BlockKind::tool) {
              for (const auto& rl : block.result_cached_lines) {
                std::string line = cont_prefix + rl;
                render_line(buffer, line, block, bg, current_y);
                current_y++;
              }
            }
          }
        } else {
          // Non-collapsible: user, text, error, warning, system
          for (const auto& cl : block.cached_lines) {
            render_line(buffer, cl, block, bg, current_y);
            current_y++;
          }
        }
      }
    }

    // --- Collapse/Expand ---

    // No longer collapses text blocks. Text blocks are always expanded.
    // Kept for API compatibility; currently a no-op.
    void complete_turn()
    {
      recalc_total_lines();
    }

    // Toggle collapse/expand of the block at the given index.
    // Only collapsible block types (thinking, tool) can be toggled.
    // Returns true if the block was toggled.
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
    // Returns -1 if the line is outside all blocks or on a separator line.
    int block_at_line(int line) const
    {
      int current = 0;
      for (int i = 0; i < (int)blocks_.size(); ++i) {
        if (i > 0 && needs_separator_before(blocks_[i-1].kind, blocks_[i].kind)) {
          if (line == current) return -1;
          current++;
        }
        int bl = block_line_count(blocks_[i]);
        if (line >= current && line < current + bl) {
          return i;
        }
        current += bl;
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
             kind == BlockKind::tool;
    }

    // Returns true if the block kind auto-collapses when the next block
    // is created (i.e. when the block is "done" streaming).
    // Tool blocks are always created collapsed, so they don't need
    // auto-collapse.
    static bool is_auto_collapsible(BlockKind kind)
    {
      return kind == BlockKind::thinking;
    }

    // Auto-collapse the last block if it is auto-collapsible.
    // Also cleans up any trailing empty text block before collapsing.
    void auto_collapse_previous()
    {
      cleanup_last_text_block();
      if (!blocks_.empty() && is_auto_collapsible(blocks_.back().kind)) {
        blocks_.back().collapsed = true;
      }
    }

    // Clean up the last text block: strip trailing newlines and remove
    // it entirely if it becomes empty. Inter-block spacing is handled
    // at render time via needs_separator_before, so text blocks no
    // longer need trailing blank lines for visual separation.
    void cleanup_last_text_block()
    {
      while (!blocks_.empty() && blocks_.back().kind == BlockKind::text) {
        std::string stripped = strip_trailing_newlines(blocks_.back().text);
        if (stripped.empty()) {
          blocks_.pop_back();
        } else {
          if (stripped != blocks_.back().text) {
            blocks_.back().text = stripped;
            blocks_.back().dirty = true;
          }
          break;
        }
      }
    }

    // Returns true if a blank separator line should be rendered between
    // the previous block and the current block. No separator between
    // consecutive collapsible blocks (thinking/tool); separator between
    // all other block-type transitions.
    static bool needs_separator_before(BlockKind prev, BlockKind curr)
    {
      if (is_collapsible(prev) && is_collapsible(curr)) return false;
      return true;
    }

    // Strip leading whitespace (spaces, newlines, tabs) from a string.
    static std::string strip_leading_ws(const std::string& s)
    {
      size_t start = s.find_first_not_of(" \n\t\r");
      if (start == std::string::npos) return "";
      return s.substr(start);
    }

    // Strip all trailing newlines (and trailing spaces/tabs) from a string.
    static std::string strip_trailing_newlines(const std::string& s)
    {
      size_t end = s.size();
      while (end > 0 && (s[end - 1] == '\n' || s[end - 1] == '\r' ||
                         s[end - 1] == ' '  || s[end - 1] == '\t')) {
        --end;
      }
      return s.substr(0, end);
    }

    // Strip all leading newlines from a string. Inter-block spacing
    // is handled at render time via needs_separator_before, so text
    // blocks don't need leading blank lines for visual separation.
    static std::string strip_leading_newlines(const std::string& s)
    {
      size_t start = 0;
      while (start < s.size() && s[start] == '\n') {
        ++start;
      }
      return s.substr(start);
    }

    // Return the indent string for collapsible blocks (2 spaces).
    static std::string indent_str()
    {
      return std::string(COLLAPSE_INDENT, ' ');
    }

    // Return the number of rendered lines for a block.
    static int block_line_count(const ContentBlock& block)
    {
      if (block.kind == BlockKind::turn_separator) {
        return 1;
      }
      if (block.collapsed) {
        return 1;
      }
      if (block.kind == BlockKind::tool) {
        return (int)block.cached_lines.size() +
               (int)block.result_cached_lines.size();
      }
      return (int)block.cached_lines.size();
    }

    // Render a blank separator line (all background) at the given y.
    void render_blank_line(cpptui::Buffer& buffer,
                           const cpptui::Color& bg, int row_y)
    {
      for (int i = 0; i < width; ++i) {
        cpptui::Cell c;
        c.content = " ";
        c.bg_color = bg;
        buffer.set(x + i, row_y, c);
      }
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
      int max_width = (width > 0 ? width : 80) - PREFIX_WIDTH;
      if (max_width < 10) max_width = 10;

      switch (block.kind) {
        case BlockKind::thinking: {
          int line_count = (int)block.cached_lines.size();
          std::string first_line;
          for (const auto& line : block.cached_lines) {
            if (!line.empty()) { first_line = line; break; }
          }
          std::string prefix = "[think] ";
          std::string suffix = " (" + std::to_string(line_count) + " lines)";
          int available = max_width - (int)prefix.size() - (int)suffix.size();
          if (available < 5) available = 5;
          if ((int)first_line.size() > available) {
            first_line = first_line.substr(0, available - 3) + "...";
          }
          return prefix + first_line + suffix;
        }
        case BlockKind::tool: {
          std::string call = strip_leading_ws(block.text);
          std::string status_suffix;
          if (!block.tool_status.empty()) {
            status_suffix = " " + block.tool_status;
          }
          if ((int)(call.size() + status_suffix.size()) > max_width) {
            int truncate_at = max_width - (int)status_suffix.size() - 3;
            if (truncate_at > 0) {
              call = call.substr(0, truncate_at) + "...";
            } else {
              call = "...";
            }
          }
          return call + status_suffix;
        }
        default:
          return block.text;
      }
    }

    // Recalculate total_lines_ from scratch, accounting for collapsed blocks.
    void recalc_total_lines()
    {
      total_lines_ = 0;
      for (int i = 0; i < (int)blocks_.size(); ++i) {
        if (i > 0 && needs_separator_before(blocks_[i-1].kind, blocks_[i].kind)) {
          total_lines_ += 1;
        }
        total_lines_ += block_line_count(blocks_[i]);
      }
      fixed_height = total_lines_;
    }

    // Maps content_type to BlockKind + theme color + bold.
    // Only called for non-tool types (tool types are handled specially
    // in append_fragment).
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
          return {BlockKind::tool,
                  cpptui::Theme::current().primary, true};
        case ct::tool_result:
          return {BlockKind::tool,
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