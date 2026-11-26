#include "doctest/doctest.h"
#include "tui/tui_terminal_colors.h"

#include <string>

TEST_CASE("osc_set_default_background emits an xterm rgb: colour")
{
  const cpptui::Color c(0xef, 0xf1, 0xf5); // Catppuccin Latte base
  CHECK(mistral::tui::osc_set_default_background(c) == "\033]11;rgb:ef/f1/f5\033\\");
}

TEST_CASE("osc_set_default_foreground emits an xterm rgb: colour")
{
  const cpptui::Color c(0x4c, 0x4f, 0x69); // Catppuccin Latte text
  CHECK(mistral::tui::osc_set_default_foreground(c) == "\033]10;rgb:4c/4f/69\033\\");
}

TEST_CASE("default colours produce no escape sequence")
{
  const cpptui::Color unset;
  REQUIRE(unset.is_default);
  CHECK(mistral::tui::osc_set_default_background(unset).empty());
  CHECK(mistral::tui::osc_set_default_foreground(unset).empty());
}

TEST_CASE("osc_reset_default_colors restores background and foreground")
{
  CHECK(mistral::tui::osc_reset_default_colors() == "\033]111\033\\\033]110\033\\");
}
