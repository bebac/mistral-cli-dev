#ifndef MISTRAL_TUI_WIDGETS_H
#define MISTRAL_TUI_WIDGETS_H

#include "cpptui.hpp"

#include <functional>
#include <string>

namespace mistral::tui
{
  // ---------------------------------------------------------------------------
  // Divider: renders a horizontal line across full width.
  // ---------------------------------------------------------------------------
  class Divider : public cpptui::Widget
  {
  public:
    Divider()
    {
      fixed_height = 1;
    }

    // When non-default, overrides the theme border color.
    cpptui::Color override_fg;

    void render(cpptui::Buffer& buffer) override
    {
      cpptui::Color fg = override_fg.is_default ? fg_color.resolve(cpptui::Theme::current().border) : override_fg;
      cpptui::Color bg = bg_color.resolve(cpptui::Theme::current().background);
      for (int i = 0; i < width; ++i)
      {
        cpptui::Cell c;
        c.content = "\xe2\x94\x80"; // ─ U+2500
        c.fg_color = fg;
        c.bg_color = bg;
        buffer.set(x + i, y, c);
      }
    }
  };

  // ---------------------------------------------------------------------------
  // FooterBar: renders three strings at left, center, and right positions.
  // ---------------------------------------------------------------------------
  class FooterBar : public cpptui::Widget
  {
  public:
    FooterBar()
    {
      fixed_height = 1;
    }

    std::string left_text;
    std::string center_text;
    std::string right_text;

    void render(cpptui::Buffer& buffer) override
    {
      cpptui::Color fg = fg_color.resolve(cpptui::Theme::current().panel_fg);
      cpptui::Color bg = bg_color.resolve(cpptui::Theme::current().panel_bg);

      // Fill background
      for (int i = 0; i < width; ++i)
      {
        cpptui::Cell c;
        c.content = " ";
        c.fg_color = fg;
        c.bg_color = bg;
        buffer.set(x + i, y, c);
      }

      auto render_text = [&](const std::string& text, int start_x) {
        auto chars = cpptui::TextHelper::prepare_text_for_render(text);
        int dx = 0;
        for (const auto& ci : chars)
        {
          if (start_x + dx >= width)
            break;
          cpptui::Cell c;
          c.content = ci.content;
          c.fg_color = fg;
          c.bg_color = bg;
          buffer.set(x + start_x + dx, y, c);
          if (ci.display_width == 2 && start_x + dx + 1 < width)
          {
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
      for (const auto& ci : center_chars)
        center_w += ci.display_width;
      render_text(center_text, 1 + (inner_w - center_w) / 2);

      // Right (1px right margin)
      auto right_chars = cpptui::TextHelper::prepare_text_for_render(right_text);
      int right_w = 0;
      for (const auto& ci : right_chars)
        right_w += ci.display_width;
      render_text(right_text, width - 1 - right_w);
    }
  };

  // ---------------------------------------------------------------------------
  // InputHint: 1-line widget showing approval key bindings.
  // Empty in normal chat mode; shows hints when approval mode is active.
  // ---------------------------------------------------------------------------
  class InputHint : public cpptui::Widget
  {
  public:
    InputHint()
    {
      fixed_height = 1;
    }

    std::string text;
    cpptui::Color hint_fg;

    void render(cpptui::Buffer& buffer) override
    {
      cpptui::Color fg = hint_fg.is_default ? fg_color.resolve(cpptui::Theme::current().foreground) : hint_fg;
      cpptui::Color bg = bg_color.resolve(cpptui::Theme::current().background);

      // Fill background
      for (int i = 0; i < width; ++i)
      {
        cpptui::Cell c;
        c.content = " ";
        c.fg_color = fg;
        c.bg_color = bg;
        buffer.set(x + i, y, c);
      }

      if (text.empty())
        return;

      // Render hint text with 1px left margin
      auto chars = cpptui::TextHelper::prepare_text_for_render(text);
      int dx = 0;
      for (const auto& ci : chars)
      {
        if (1 + dx >= width)
          break;
        cpptui::Cell c;
        c.content = ci.content;
        c.fg_color = fg;
        c.bg_color = bg;
        buffer.set(x + 1 + dx, y, c);
        if (ci.display_width == 2 && 1 + dx + 1 < width)
        {
          cpptui::Cell skip;
          skip.content = "";
          skip.bg_color = bg;
          buffer.set(x + 1 + dx + 1, y, skip);
        }
        dx += ci.display_width;
      }
    }
  };

} // namespace mistral::tui

#endif // MISTRAL_TUI_WIDGETS_H
