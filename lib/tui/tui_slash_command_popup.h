#ifndef MISTRAL_TUI_SLASH_COMMAND_POPUP_H
#define MISTRAL_TUI_SLASH_COMMAND_POPUP_H

#include "cpptui.hpp"
#include "slash_command/command_spec.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace mistral::tui
{

  // ---------------------------------------------------------------------------
  // SlashCommandPopup: bordered popup showing completion candidates.
  // Renders a 2-column table: <prefix><text> | description.
  // Selected row highlighted with Theme::selection background.
  //
  // The popup is a dumb list: the caller supplies the already filtered
  // candidates (see slash_command_registry::complete), so the same widget
  // serves command names and command arguments.
  // ---------------------------------------------------------------------------

  class SlashCommandPopup : public cpptui::Widget
  {
  public:
    static constexpr int max_visible = 8;
    static constexpr int col1_width = 20;

    SlashCommandPopup()
    {
      focusable = false;
    }

    // Replace the candidate list. Resets the selection to the first item.
    void set_items(std::vector<mistral::slash_command::completion_item> items)
    {
      items_ = std::move(items);
      selected_index_ = 0;
    }

    // Prefix rendered before each candidate: "/" for command names, "" for
    // command arguments.
    void set_display_prefix(std::string prefix)
    {
      display_prefix_ = std::move(prefix);
    }

    void move_selection(int delta)
    {
      if (items_.empty())
        return;
      selected_index_ = (selected_index_ + delta + (int)items_.size()) % (int)items_.size();
    }

    std::string selected_text() const
    {
      if (selected_index_ >= 0 && selected_index_ < (int)items_.size())
      {
        return items_[selected_index_].text;
      }
      return "";
    }

    int count() const
    {
      return (int)items_.size();
    }

    int popup_height() const
    {
      int rows = std::min(count(), max_visible);
      if (rows == 0)
        rows = 1;      // "No matching commands"
      return rows + 2; // +2 for top/bottom border
    }

    void render(cpptui::Buffer& buffer) override
    {
      using namespace cpptui;

      Color border_fg = fg_color.resolve(Theme::current().border);
      Color sel_bg = Theme::current().selection;
      Color sel_fg = Theme::current().input_fg;
      Color norm_bg = Theme::current().panel_bg;
      Color norm_fg = Theme::current().foreground;

      int w = width;
      int h = height;
      if (w < 2 || h < 2)
        return;

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
        for (int i = 1; i < w - 1; ++i)
        {
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
        for (int i = 1; i < w - 1; ++i)
        {
          buffer.set(x + i, y + h - 1, c);
        }
      }

      // Side borders for all content rows
      for (int row = 0; row < inner_h; ++row)
      {
        Cell vc;
        vc.fg_color = border_fg;
        vc.content = "\xe2\x94\x82"; // │
        buffer.set(x, y + 1 + row, vc);
        buffer.set(x + w - 1, y + 1 + row, vc);
      }

      if (items_.empty())
      {
        // "No matching commands"
        for (int i = 1; i < w - 1; ++i)
        {
          Cell c;
          c.content = " ";
          c.fg_color = norm_fg;
          c.bg_color = norm_bg;
          buffer.set(x + i, y + 1, c);
        }
        std::string msg = "No matching commands";
        auto chars = TextHelper::prepare_text_for_render(msg);
        int dx = 0;
        for (const auto& ci : chars)
        {
          if (1 + dx >= w - 1)
            break;
          Cell c;
          c.content = ci.content;
          c.fg_color = norm_fg;
          c.bg_color = norm_bg;
          buffer.set(x + 1 + dx, y + 1, c);
          if (ci.display_width == 2 && 1 + dx + 1 < w - 1)
          {
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
      if ((int)items_.size() > max_visible && selected_index_ >= max_visible)
      {
        scroll_offset = selected_index_ - max_visible + 1;
      }

      int shown = std::min((int)items_.size() - scroll_offset, max_visible);
      shown = std::min(shown, inner_h);

      for (int row = 0; row < shown; ++row)
      {
        int item_idx = scroll_offset + row;
        int abs_y = y + 1 + row;
        bool selected = (item_idx == selected_index_);
        Color bg = selected ? sel_bg : norm_bg;
        Color fg = selected ? sel_fg : norm_fg;

        // Fill row background
        for (int i = 1; i < w - 1; ++i)
        {
          Cell c;
          c.content = " ";
          c.fg_color = fg;
          c.bg_color = bg;
          buffer.set(x + i, abs_y, c);
        }

        // Column 1: <prefix><text>
        std::string col1 = display_prefix_ + items_[item_idx].text;
        auto chars1 = TextHelper::prepare_text_for_render(col1);
        int dx = 0;
        for (const auto& ci : chars1)
        {
          if (1 + dx >= w - 1 || dx >= col1_width)
            break;
          Cell c;
          c.content = ci.content;
          c.fg_color = fg;
          c.bg_color = bg;
          buffer.set(x + 1 + dx, abs_y, c);
          if (ci.display_width == 2 && 1 + dx + 1 < w - 1 && dx + 1 < col1_width)
          {
            Cell skip;
            skip.content = "";
            skip.bg_color = bg;
            buffer.set(x + 1 + dx + 1, abs_y, skip);
          }
          dx += ci.display_width;
        }

        // Column 2: description
        const std::string& desc = items_[item_idx].description;
        int col2_start = 1 + col1_width;
        int col2_width = inner_w - col1_width;
        if (col2_width > 0 && !desc.empty())
        {
          auto chars2 = TextHelper::prepare_text_for_render(desc);
          int total_w = 0;
          for (const auto& ci : chars2)
            total_w += ci.display_width;

          if (total_w > col2_width)
          {
            // Truncate with …
            int remaining_w = col2_width - 1;
            int dx2 = 0;
            for (const auto& ci : chars2)
            {
              if (remaining_w < ci.display_width)
                break;
              if (col2_start + dx2 >= w - 1)
                break;
              Cell c;
              c.content = ci.content;
              c.fg_color = fg;
              c.bg_color = bg;
              buffer.set(x + col2_start + dx2, abs_y, c);
              if (ci.display_width == 2 && col2_start + dx2 + 1 < w - 1)
              {
                Cell skip;
                skip.content = "";
                skip.bg_color = bg;
                buffer.set(x + col2_start + dx2 + 1, abs_y, skip);
              }
              dx2 += ci.display_width;
              remaining_w -= ci.display_width;
            }
            if (col2_start + dx2 < w - 1)
            {
              Cell c;
              c.content = "\xe2\x80\xa6"; // …
              c.fg_color = fg;
              c.bg_color = bg;
              buffer.set(x + col2_start + dx2, abs_y, c);
            }
          }
          else
          {
            int dx2 = 0;
            for (const auto& ci : chars2)
            {
              if (col2_start + dx2 >= w - 1)
                break;
              Cell c;
              c.content = ci.content;
              c.fg_color = fg;
              c.bg_color = bg;
              buffer.set(x + col2_start + dx2, abs_y, c);
              if (ci.display_width == 2 && col2_start + dx2 + 1 < w - 1)
              {
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
      for (int row = shown; row < inner_h; ++row)
      {
        int abs_y = y + 1 + row;
        for (int i = 1; i < w - 1; ++i)
        {
          Cell c;
          c.content = " ";
          c.bg_color = norm_bg;
          buffer.set(x + i, abs_y, c);
        }
      }
    }

  private:
    std::vector<mistral::slash_command::completion_item> items_;
    std::string display_prefix_;
    int selected_index_ = 0;
  };

} // namespace mistral::tui

#endif // MISTRAL_TUI_SLASH_COMMAND_POPUP_H
