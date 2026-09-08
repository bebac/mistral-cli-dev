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
    system,              // system messages
    turn_separator,      // "---[N]---" full-width separator
  };

  // ---------------------------------------------------------------------------
  // ToolApprovalState: approval lifecycle state for tool blocks.
  // ---------------------------------------------------------------------------
  enum class ToolApprovalState {
    none,               // No approval needed
    pending,            // Waiting for user approval
    approved,           // User approved
    rejected            // User rejected
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
  //
  // Approval state: tool blocks can have an approval state machine for
  // tracking tool approval lifecycle. When approval_state is `pending`,
  // the approval_prompt is displayed within the tool block.
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
    mutable std::vector<std::string> approval_cached_lines; // for approval prompt
    mutable int cached_width = 0;
    mutable int result_cached_width = 0;
    mutable int approval_cached_width = 0;
    bool dirty = false;
    bool result_dirty = false;
    bool approval_dirty = false;
    std::string tool_status;       // "" = none, "✓" or "✗" for tool blocks

    // Approval state machine for tool blocks
    ToolApprovalState approval_state = ToolApprovalState::none;
    std::string approval_prompt;   // The formatted approval prompt text
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
  // Render uses virtualized rendering: it queries the buffer's current
  // clip rect to determine the visible line range, then binary-searches
  // a cached block-offset array to find the first visible block. Only
  // visible lines of visible blocks are rendered — O(log(num_blocks) +
  // visible_lines) instead of O(total_lines). Blocks entirely above or
  // below the viewport are skipped entirely.
  //
  // The cached block-offset array (block_offsets_) stores the starting
  // content line of each block (including separator lines). It is
  // invalidated whenever blocks change (via recalc_total_lines()) and
  // rebuilt on demand in ensure_block_offsets().
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

    ContentLog() { focusable = true; tab_stop = false; }

    // O(1) accumulation of a text fragment into the last block (if same kind)
    // or a new block. Does NOT re-wrap — just accumulates text.
    //
    // tool_call: always creates a new tool block. Leading whitespace is
    //   stripped from the fragment.
    // tool_approval: attaches to the last tool block (one with empty result_text)
    //   and sets its approval_state to pending, expanded by default.
    // tool_result: appends to the last tool block's result_text and sets
    //   its status to ✓. If no pending tool block exists, creates one.
    // Other types: merge with last block if same kind, else create new.
    //
    // When force_new is true, always creates a new block even if the last
    // block has the same kind (except for tool_result and tool_approval,
    // which always append to the last tool block).
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

      // --- Tool approval: attach to the last tool block awaiting approval ---
      if (type == content_type::tool_approval) {
        // Search backwards for the last tool block without a result
        // (one with empty result_text and no approval state set).
        for (int i = (int)blocks_.size() - 1; i >= 0; --i) {
          if (blocks_[i].kind == BlockKind::tool &&
              blocks_[i].result_text.empty() &&
              blocks_[i].approval_state == ToolApprovalState::none) {
            // Attach approval prompt to this tool block
            blocks_[i].approval_prompt = fragment;
            blocks_[i].approval_state = ToolApprovalState::pending;
            blocks_[i].collapsed = false;  // Expanded by default for approval
            blocks_[i].dirty = true;
            blocks_[i].approval_dirty = true;
            return;
          }
        }
        // Edge case: no pending tool block — create a tool block with approval
        // This shouldn't normally happen, but handle it gracefully.
        auto_collapse_previous();
        ContentBlock block;
        block.kind = BlockKind::tool;
        block.color = cpptui::Theme::current().primary;
        block.bold = true;
        block.text = "[tool] unknown";
        block.approval_prompt = fragment;
        block.approval_state = ToolApprovalState::pending;
        block.collapsed = false;
        block.dirty = true;
        block.approval_dirty = true;
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
          // Clear pending approval state and auto-collapse
          if (blocks_.back().approval_state == ToolApprovalState::pending) {
            blocks_.back().approval_state = ToolApprovalState::approved;
            blocks_.back().collapsed = true;
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
            // Clear pending approval state and auto-collapse
            if (blocks_[i].approval_state == ToolApprovalState::pending) {
              blocks_[i].approval_state = ToolApprovalState::approved;
              blocks_[i].collapsed = true;
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
        // Only strip leading newlines at block-type boundaries (when
        // the previous block is a different kind). This normalizes
        // spacing between tool call, thinking, and text blocks. Don't
        // strip for the first block or when force_new creates a
        // same-kind block — those newlines are part of the content.
        if (style.kind == BlockKind::text && !blocks_.empty() &&
            blocks_.back().kind != BlockKind::text) {
          block.text = strip_leading_newlines(block.text);
        }
        block.color = style.color;
        block.bold = style.bold;
        block.dirty = true;
        blocks_.push_back(std::move(block));
      } else {
        blocks_.back().text += fragment;
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
        if (block.approval_dirty) {
          block.approval_cached_lines = cpptui::Paragraph::wrap_text_static(
            block.approval_prompt, wrap_w, 0, 0, true);
          block.approval_cached_width = wrap_w;
          block.approval_dirty = false;
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
      if (width <= 0 || blocks_.empty()) return;
      cpptui::Color bg = cpptui::Theme::current().background;

      // Determine the visible content line range from the buffer's
      // current clip rect. The clip is the intersection of the parent
      // ScrollableVertical's viewport and this widget's area, so
      // vis_top/vis_bottom are content-line indices (0-based from the
      // top of the content).
      cpptui::Rect clip = buffer.current_clip();
      int vis_top = clip.y - y;
      int vis_bottom = clip.y + clip.height - 1 - y;

      // Entire content is outside the viewport.
      if (vis_bottom < 0 || vis_top >= total_lines_) return;
      if (vis_top < 0) vis_top = 0;

      ensure_block_offsets();
      ensure_flat_text();

      // Binary search for the first block whose last line >= vis_top.
      // block_offsets_[i] is the starting content line of block i.
      int first_block = 0;
      {
        int lo = 0, hi = (int)blocks_.size() - 1;
        while (lo <= hi) {
          int mid = lo + (hi - lo) / 2;
          int end = block_offsets_[mid] +
                    block_line_count(blocks_[mid]) - 1;
          if (end >= vis_top) {
            first_block = mid;
            hi = mid - 1;
          } else {
            lo = mid + 1;
          }
        }
      }

      // Render visible blocks, skipping lines outside the viewport.
      for (int i = first_block; i < (int)blocks_.size(); ++i) {
        const auto& block = blocks_[i];
        int block_start = block_offsets_[i];

        // Separator line before this block (at block_start - 1).
        if (i > 0 && needs_separator_before(blocks_[i-1].kind, block.kind)) {
          int sep_line = block_start - 1;
          if (sep_line >= vis_top && sep_line <= vis_bottom) {
            render_blank_line(buffer, bg, y + sep_line);
          }
        }

        // Block is entirely below the viewport — done.
        if (block_start > vis_bottom) break;

        // Lines to skip at the top of this block (partial visibility).
        int skip = std::max(0, vis_top - block_start);

        if (block.kind == BlockKind::turn_separator) {
          if (skip == 0) {
            std::string line = block.text;
            while ((int)line.size() < width) line += '-';
            render_line(buffer, line, block, bg, y + block_start,
                        block_char_offsets_[i]);
          }
        } else if (is_collapsible(block.kind)) {
          if (block.collapsed) {
            if (skip == 0) {
              std::string line = indent_str() + COLLAPSED_IND + " " +
                                 generate_summary(block);
              render_line(buffer, line, block, bg, y + block_start,
                          block_char_offsets_[i], PREFIX_WIDTH);
            }
          } else {
            const std::string first_prefix = indent_str() +
              std::string(EXPANDED_IND) + " ";
            const std::string cont_prefix = std::string(PREFIX_WIDTH, ' ');
            int li = 0;
            int line_char_offset = 0;
            for (const auto& cl : block.cached_lines) {
              if (li >= skip && block_start + li <= vis_bottom) {
                std::string line = (li == 0 ? first_prefix : cont_prefix) + cl;
                render_line(buffer, line, block, bg, y + block_start + li,
                            block_char_offsets_[i] + line_char_offset,
                            PREFIX_WIDTH);
              }
              line_char_offset +=
                (int)cpptui::TextHelper::count_codepoints(cl) + 1;
              li++;
            }
            // Tool blocks: render approval prompt and result lines
            if (block.kind == BlockKind::tool) {
              // Render approval prompt if pending
              if (block.approval_state == ToolApprovalState::pending &&
                  !block.approval_prompt.empty()) {
                ContentBlock prompt_block;
                prompt_block.color = cpptui::Theme::current().warning;
                prompt_block.bold = false;
                for (const auto& al : block.approval_cached_lines) {
                  if (li >= skip && block_start + li <= vis_bottom) {
                    std::string line = cont_prefix + al;
                    render_line(buffer, line, prompt_block, bg,
                                y + block_start + li,
                                block_char_offsets_[i] + line_char_offset,
                                PREFIX_WIDTH);
                  }
                  line_char_offset +=
                    (int)cpptui::TextHelper::count_codepoints(al) + 1;
                  li++;
                }
              }
              // Render result lines
              int result_base = block_char_offsets_[i] +
                line_char_offset;
              int result_line_char_offset = 0;
              for (const auto& rl : block.result_cached_lines) {
                if (li >= skip && block_start + li <= vis_bottom) {
                  std::string line = cont_prefix + rl;
                  render_line(buffer, line, block, bg, y + block_start + li,
                              result_base + result_line_char_offset,
                              PREFIX_WIDTH);
                }
                result_line_char_offset +=
                  (int)cpptui::TextHelper::count_codepoints(rl) + 1;
                li++;
              }
            }
          }
        } else {
          // Non-collapsible: user, text, error, warning, system
          int li = 0;
          int line_char_offset = 0;
          for (const auto& cl : block.cached_lines) {
            if (li >= skip && block_start + li <= vis_bottom) {
              render_line(buffer, cl, block, bg, y + block_start + li,
                          block_char_offsets_[i] + line_char_offset);
            }
            line_char_offset +=
              (int)cpptui::TextHelper::count_codepoints(cl) + 1;
            li++;
          }
        }
      }
    }

    // --- Collapse/Expand ---

    // Finalize the current turn: auto-collapse the last block if it is
    // an auto-collapsible type (thinking), clean up any trailing empty
    // text block, and recalculate total lines. Called by content_area
    // when the render_sink's completed() callback fires.
    //
    // Text blocks are always expanded and are NOT collapsed here.
    // Tool blocks are always created collapsed, so no action needed.
    // The last thinking block of a turn may still be expanded (no
    // subsequent block triggered auto_collapse_previous), so this
    // method ensures it gets collapsed.
    void complete_turn()
    {
      auto_collapse_previous();
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
      offsets_valid_ = false;
      flat_text_valid_ = false;
    }

    // --- Text selection ---

    bool selectable = true;

    bool has_selection() const override
    {
      return selection_state_.has_selection();
    }

    std::string get_selected_text() const
    {
      if (!has_selection()) return "";
      ensure_flat_text();
      int s, e;
      selection_state_.get_range(s, e);
      return cpptui::TextHelper::utf8_substr(flat_text_, s, e - s);
    }

    void clear_selection() { selection_state_.clear(); }

    void on_blur() override
    {
      selection_state_.clear();
      Widget::on_blur();
    }

    bool on_event(const cpptui::Event& event) override
    {
      if (!selectable) return false;
      bool hit = contains(event.x, event.y);

      if (event.is_mouse_event()) {
        if (hit && event.mouse_left() && !event.mouse_motion()) {
          set_focus(true);
          ensure_flat_text();
          int new_pos = visual_to_char_idx(event.x, event.y);
          auto chars =
            cpptui::TextHelper::prepare_text_for_render(flat_text_);
          selection_state_.handle_mouse_press(chars, new_pos,
                                               event.click_count);
          return true;
        }
        if (selection_state_.mouse_down && event.mouse_drag()) {
          selection_state_.handle_mouse_drag(
            visual_to_char_idx(event.x, event.y));
          return true;
        }
        if (event.mouse_release()) {
          selection_state_.handle_mouse_release();
        }
        if (hit && event.mouse_right()) {
          if (has_selection())
            cpptui::copy_to_clipboard(get_selected_text());
          return true;
        }
        return false;
      }

      if (has_focus() && event.is_key_event()) {
        if (event.is_select_all()) {
          ensure_flat_text();
          selection_state_.start = 0;
          selection_state_.end =
            (int)cpptui::TextHelper::count_codepoints(flat_text_);
          return true;
        }
        if (event.is_copy()) {
          if (has_selection())
            cpptui::copy_to_clipboard(get_selected_text());
          return true;
        }
      }
      return false;
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
        int count = (int)block.cached_lines.size() +
                    (int)block.result_cached_lines.size();
        if (block.approval_state == ToolApprovalState::pending &&
            !block.approval_prompt.empty()) {
          count += (int)block.approval_cached_lines.size();
        }
        return count;
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
                     const cpptui::Color& bg, int row_y,
                     int sel_char_offset = -1, int sel_skip_chars = 0)
    {
      cpptui::Color fg = block.color.is_default
        ? cpptui::Theme::current().foreground
        : block.color;
      auto chars = cpptui::TextHelper::prepare_text_for_render(line);
      int dx = 0;
      for (size_t ci_idx = 0; ci_idx < chars.size(); ++ci_idx) {
        const auto& ci = chars[ci_idx];
        if (dx >= width) break;
        cpptui::Cell c;
        c.content = ci.content;
        c.fg_color = fg;
        c.bg_color = bg;
        c.bold = block.bold;
        if (sel_char_offset >= 0 && (int)ci_idx >= sel_skip_chars) {
          int global_idx = sel_char_offset + ((int)ci_idx - sel_skip_chars);
          if (selection_state_.is_selected(global_idx)) {
            c.bg_color = cpptui::Theme::current().selection;
            c.fg_color = cpptui::Color::contrast_color(c.bg_color);
          }
        }
        buffer.set(x + dx, row_y, c);
        if (ci.display_width == 2 && dx + 1 < width) {
          cpptui::Cell skip;
          skip.content = "";
          skip.bg_color = c.bg_color;
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
          // Add approval state indicator for collapsed blocks
          std::string approval_suffix;
          if (block.approval_state == ToolApprovalState::pending) {
            approval_suffix = " [AWAITING APPROVAL]";
          } else if (block.approval_state == ToolApprovalState::approved) {
            approval_suffix = " [APPROVED]";
          } else if (block.approval_state == ToolApprovalState::rejected) {
            approval_suffix = " [REJECTED]";
          }
          
          // Combine all suffixes
          std::string all_suffix = status_suffix + approval_suffix;
          
          if ((int)(call.size() + all_suffix.size()) > max_width) {
            int truncate_at = max_width - (int)all_suffix.size() - 3;
            if (truncate_at > 0) {
              call = call.substr(0, truncate_at) + "...";
            } else {
              call = "...";
            }
          }
          return call + all_suffix;
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
      offsets_valid_ = false;
      flat_text_valid_ = false;
    }

    // Rebuild the cached block-offset array if invalidated.
    // block_offsets_[i] stores the starting content line of block i,
    // accounting for separator lines. Called from render() before the
    // virtualized loop.
    void ensure_block_offsets()
    {
      if (offsets_valid_) return;
      block_offsets_.resize(blocks_.size());
      int current = 0;
      for (int i = 0; i < (int)blocks_.size(); ++i) {
        if (i > 0 && needs_separator_before(blocks_[i-1].kind, blocks_[i].kind)) {
          current += 1;  // separator line before this block
        }
        block_offsets_[i] = current;
        current += block_line_count(blocks_[i]);
      }
      offsets_valid_ = true;
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
        case ct::tool_approval:
          // tool_approval is handled specially in append_fragment, but we
          // need this case to silence compiler warnings. Should not be reached.
          return {BlockKind::tool,
                  cpptui::Theme::current().warning, false};
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

    // Cached block line offsets for virtualized rendering.
    // block_offsets_[i] = starting content line of block i (including
    // separator lines). Invalidated by recalc_total_lines() and clear().
    std::vector<int> block_offsets_;
    bool offsets_valid_ = false;

    // --- Text selection ---
    cpptui::SelectionState selection_state_;

    // Flat text cache for selection (concatenation of all block texts
    // with newlines between them). Invalidated whenever blocks change
    // (via recalc_total_lines); rebuilt on demand.
    mutable std::string flat_text_;
    mutable std::vector<int> block_char_offsets_;
    mutable bool flat_text_valid_ = false;

    // Rebuild the flat text and block char offsets if invalidated.
    // The flat text is the concatenation of all block texts with
    // newlines between them. For collapsed blocks, the summary is used.
    // For tool blocks (expanded), call text + "\n" + result text.
    // Turn separators contribute their text. block_char_offsets_[i]
    // stores the starting char index of block i in flat_text_.
    void ensure_flat_text() const
    {
      if (flat_text_valid_) return;
      flat_text_.clear();
      block_char_offsets_.assign(blocks_.size(), 0);

      for (int i = 0; i < (int)blocks_.size(); ++i) {
        if (i > 0) flat_text_ += "\n";
        block_char_offsets_[i] =
          (int)cpptui::TextHelper::count_codepoints(flat_text_);

        if (blocks_[i].kind == BlockKind::turn_separator) {
          flat_text_ += blocks_[i].text;
          continue;
        }

        if (is_collapsible(blocks_[i].kind) && blocks_[i].collapsed) {
          flat_text_ += generate_summary(blocks_[i]);
        } else {
          // Build flat text from cached_lines (joined with \n) instead
          // of raw block.text.  The cached_lines are the wrapped version
          // of block.text — they don't contain the \n characters that
          // separate paragraphs in the original text.  By building the
          // flat text from cached_lines, the character offsets used in
          // visual_to_char_idx and render (which accumulate codepoint
          // counts of cached_lines + 1 per \n) are consistent with the
          // flat text.
          if (blocks_[i].cached_lines.empty()) {
            // Block not yet flushed — fall back to raw text
            flat_text_ += blocks_[i].text;
            if (blocks_[i].kind == BlockKind::tool &&
                !blocks_[i].result_text.empty()) {
              flat_text_ += "\n";
              flat_text_ += blocks_[i].result_text;
            }
          } else {
            for (size_t li = 0;
                 li < blocks_[i].cached_lines.size(); ++li) {
              if (li > 0) flat_text_ += "\n";
              flat_text_ += blocks_[i].cached_lines[li];
            }
            if (blocks_[i].kind == BlockKind::tool &&
                blocks_[i].approval_state == ToolApprovalState::pending &&
                !blocks_[i].approval_prompt.empty()) {
              for (size_t ai = 0;
                   ai < blocks_[i].approval_cached_lines.size(); ++ai) {
                flat_text_ += "\n";
                flat_text_ += blocks_[i].approval_cached_lines[ai];
              }
            }
            if (blocks_[i].kind == BlockKind::tool &&
                !blocks_[i].result_text.empty()) {
              flat_text_ += "\n";
              for (size_t ri = 0;
                   ri < blocks_[i].result_cached_lines.size(); ++ri) {
                if (ri > 0) flat_text_ += "\n";
                flat_text_ += blocks_[i].result_cached_lines[ri];
              }
            }
          }
        }
      }
      flat_text_valid_ = true;
    }

    // Map screen coordinates (vx, vy) to a character index in
    // flat_text_. Clamps to [0, flat_text codepoint count].
    int visual_to_char_idx(int vx, int vy)
    {
      ensure_flat_text();
      ensure_block_offsets();

      int content_line = vy - y;
      if (content_line < 0) return 0;

      for (int i = 0; i < (int)blocks_.size(); ++i) {
        int block_start = block_offsets_[i];
        int block_end = block_start +
          block_line_count(blocks_[i]) - 1;

        // Separator line before this block
        if (i > 0 && needs_separator_before(blocks_[i-1].kind,
                                            blocks_[i].kind)) {
          if (content_line == block_start - 1) {
            return block_char_offsets_[i] > 0
              ? block_char_offsets_[i] - 1 : 0;
          }
        }

        if (content_line >= block_start &&
            content_line <= block_end) {
          int line_within = content_line - block_start;
          int char_offset = block_char_offsets_[i];

          if (blocks_[i].kind == BlockKind::turn_separator) {
            int rel_x = vx - x;
            auto chars = cpptui::TextHelper::prepare_text_for_render(
              blocks_[i].text);
            int idx = cpptui::TextHelper::visual_to_char_pos(
              chars, rel_x);
            return char_offset + idx;
          }

          if (is_collapsible(blocks_[i].kind) &&
              blocks_[i].collapsed) {
            std::string summary = generate_summary(blocks_[i]);
            int rel_x = vx - x - PREFIX_WIDTH;
            if (rel_x < 0) rel_x = 0;
            auto chars =
              cpptui::TextHelper::prepare_text_for_render(summary);
            int idx = cpptui::TextHelper::visual_to_char_pos(
              chars, rel_x);
            return char_offset + idx;
          }

          // Expanded block — find the line within cached_lines
          int line_char_offset = 0;
          for (int li = 0;
               li < (int)blocks_[i].cached_lines.size(); ++li) {
            if (li == line_within) {
              int rel_x;
              if (is_collapsible(blocks_[i].kind)) {
                rel_x = vx - x - PREFIX_WIDTH;
              } else {
                rel_x = vx - x;
              }
              if (rel_x < 0) rel_x = 0;
              auto chars =
                cpptui::TextHelper::prepare_text_for_render(
                  blocks_[i].cached_lines[li]);
              int idx = cpptui::TextHelper::visual_to_char_pos(
                chars, rel_x);
              return char_offset + line_char_offset + idx;
            }
            line_char_offset +=
              (int)cpptui::TextHelper::count_codepoints(
                blocks_[i].cached_lines[li]) + 1;
          }

          // Tool blocks: check approval prompt and result lines
          if (blocks_[i].kind == BlockKind::tool) {
            int call_lines =
              (int)blocks_[i].cached_lines.size();
            int approval_lines =
              (blocks_[i].approval_state == ToolApprovalState::pending &&
               !blocks_[i].approval_prompt.empty())
                ? (int)blocks_[i].approval_cached_lines.size() : 0;
            // Check if the click is on an approval prompt line
            if (approval_lines > 0 &&
                line_within >= call_lines &&
                line_within < call_lines + approval_lines) {
              int approval_line = line_within - call_lines;
              int rel_x = vx - x - PREFIX_WIDTH;
              if (rel_x < 0) rel_x = 0;
              auto chars = cpptui::TextHelper::prepare_text_for_render(
                blocks_[i].approval_cached_lines[approval_line]);
              int idx = cpptui::TextHelper::visual_to_char_pos(
                chars, rel_x);
              return char_offset + line_char_offset + idx;
            }
            int result_line = line_within - call_lines - approval_lines;
            if (result_line >= 0 && result_line <
                (int)blocks_[i].result_cached_lines.size()) {
              int rel_x = vx - x - PREFIX_WIDTH;
              if (rel_x < 0) rel_x = 0;
              int result_base = char_offset + line_char_offset;
              for (int ai = 0; ai < approval_lines; ++ai) {
                result_base += (int)cpptui::TextHelper::count_codepoints(
                  blocks_[i].approval_cached_lines[ai]) + 1;
              }
              int result_line_char_offset = 0;
              for (int ri = 0; ri <= result_line; ++ri) {
                if (ri == result_line) {
                  auto chars =
                    cpptui::TextHelper::prepare_text_for_render(
                      blocks_[i].result_cached_lines[ri]);
                  int idx =
                    cpptui::TextHelper::visual_to_char_pos(
                      chars, rel_x);
                  return result_base +
                    result_line_char_offset + idx;
                }
                result_line_char_offset +=
                  (int)cpptui::TextHelper::count_codepoints(
                    blocks_[i].result_cached_lines[ri]) + 1;
              }
            }
          }

          // Fallback: end of this block's text
          return char_offset +
            std::max(0, line_char_offset - 1);
        }
      }

      return (int)cpptui::TextHelper::count_codepoints(flat_text_);
    }
  };

} // namespace mistral::tui

#endif // MISTRAL_TUI_CONTENT_LOG_H