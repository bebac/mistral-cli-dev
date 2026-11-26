#ifndef MISTRAL_TUI_TERMINAL_COLORS_H
#define MISTRAL_TUI_TERMINAL_COLORS_H

#include "cpptui.hpp"

#include <format>
#include <string>

namespace mistral::tui
{
  // -------------------------------------------------------------------------
  // Terminal default colours.
  //
  // The TUI paints every cell of its own grid, but the terminal still draws the
  // area around that grid - window padding, and any cell outside the painted
  // region - with the profile's default background. When the profile and the
  // theme disagree (a dark terminal running the light theme, say) that area
  // shows up as a border in the wrong colour.
  //
  // OSC 11 / OSC 10 set the default background / foreground for the session, so
  // the area the terminal draws itself matches the theme. OSC 111 / OSC 110 put
  // the profile's own colours back.
  // -------------------------------------------------------------------------

  /// @brief OSC 11: set the terminal's default background colour.
  /// @param c The colour to use. A default (unset) colour yields an empty
  ///          string, since there is nothing meaningful to send.
  /// @return The escape sequence, or an empty string for a default colour.
  inline std::string osc_set_default_background(const cpptui::Color& c)
  {
    if (c.is_default)
      return {};

    return std::format(
      "\033]11;rgb:{:02x}/{:02x}/{:02x}\033\\",
      static_cast<unsigned>(c.r),
      static_cast<unsigned>(c.g),
      static_cast<unsigned>(c.b)
    );
  }

  /// @brief OSC 10: set the terminal's default foreground colour.
  /// @param c The colour to use. A default (unset) colour yields an empty
  ///          string, since there is nothing meaningful to send.
  /// @return The escape sequence, or an empty string for a default colour.
  inline std::string osc_set_default_foreground(const cpptui::Color& c)
  {
    if (c.is_default)
      return {};

    return std::format(
      "\033]10;rgb:{:02x}/{:02x}/{:02x}\033\\",
      static_cast<unsigned>(c.r),
      static_cast<unsigned>(c.g),
      static_cast<unsigned>(c.b)
    );
  }

  /// @brief OSC 111 + OSC 110: restore the profile's default background and
  /// foreground.
  /// @return The escape sequence that resets both defaults.
  inline std::string osc_reset_default_colors()
  {
    return "\033]111\033\\\033]110\033\\";
  }

} // namespace mistral::tui

#endif // MISTRAL_TUI_TERMINAL_COLORS_H
