#ifndef MISTRAL_TUI_SLASH_COMMAND_POPUP_H
#define MISTRAL_TUI_SLASH_COMMAND_POPUP_H

#include "cpptui.hpp"
#include "slash_command/slash_command_registry.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace mistral::tui
{

  // ---------------------------------------------------------------------------
  // SlashCommandPopup: bordered popup showing filtered slash commands.
  // Renders a 2-column table: /command_name | description.
  // Selected row highlighted with Theme::selection background.
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

    void set_registry(const mistral::slash_command::slash_command_registry* registry)
    {
      registry_ = registry;
    }

    void set_filter(const std::string& filter)
    {
      filter_ = filter;
      update_matched_commands();
      selected_index_ = 0;
    }

    void move_selection(int delta)
    {
      if (matched_commands_.empty())
        return;
      selected_index_ = (selected_index_ + delta + (int)matched_commands_.size()) % (int)matched_commands_.size();
    }

    std::string selected_command_name() const
    {
      if (selected_index_ >= 0 && selected_index_ < (int)matched_commands_.size())
      {
        return matched_commands_[selected_index_];
      }
      return "";
    }

    int matched_count() const
    {
      return (int)matched_commands_.size();
    }

    int popup_height() const
    {
      int rows = std::min(matched_count(), max_visible);
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

      if (matched_commands_.empty())
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
      if ((int)matched_commands_.size() > max_visible && selected_index_ >= max_visible)
      {
        scroll_offset = selected_index_ - max_visible + 1;
      }

      int shown = std::min((int)matched_commands_.size() - scroll_offset, max_visible);
      shown = std::min(shown, inner_h);

      for (int row = 0; row < shown; ++row)
      {
        int cmd_idx = scroll_offset + row;
        int abs_y = y + 1 + row;
        bool selected = (cmd_idx == selected_index_);
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

        // Column 1: /command_name
        std::string col1 = "/" + matched_commands_[cmd_idx];
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
        const auto* cmd = registry_->get_command(matched_commands_[cmd_idx]);
        std::string desc = cmd ? cmd->description() : "";
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
    void update_matched_commands()
    {
      matched_commands_.clear();
      if (!registry_)
        return;
      auto names = registry_->get_command_names();
      for (const auto& name : names)
      {
        if (filter_.empty() || starts_with_ci(name, filter_))
        {
          matched_commands_.push_back(name);
        }
      }
    }

    static bool starts_with_ci(const std::string& str, const std::string& prefix)
    {
      if (prefix.size() > str.size())
        return false;
      for (size_t i = 0; i < prefix.size(); ++i)
      {
        if (std::tolower(static_cast<unsigned char>(str[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
        {
          return false;
        }
      }
      return true;
    }

    const mistral::slash_command::slash_command_registry* registry_ = nullptr;
    std::string filter_;
    std::vector<std::string> matched_commands_;
    int selected_index_ = 0;
  };

} // namespace mistral::tui

#endif // MISTRAL_TUI_SLASH_COMMAND_POPUP_H
