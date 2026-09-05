#include "tui/content_log.h"
#include "doctest/doctest.h"

#include <cpptui.hpp>

using content_type = mistral::agent::content_type;
using BlockKind = mistral::tui::BlockKind;

TEST_SUITE_BEGIN("content_log");

// ---------------------------------------------------------------------------
// Block creation and kind mapping
// ---------------------------------------------------------------------------

TEST_CASE("append_fragment creates block with correct kind for each type") {
  mistral::tui::ContentLog log;
  log.append_fragment("hello", content_type::normal);
  log.append_fragment("thinking...", content_type::thinking);
  log.append_fragment("[tool] read()", content_type::tool_call);
  log.append_fragment("-> result", content_type::tool_result);
  log.append_fragment("[error] oops", content_type::error);
  log.append_fragment("[warning] careful", content_type::warning);
  log.append_fragment("> user", content_type::user);
  log.append_fragment("system msg", content_type::system);

  REQUIRE(log.block_count() == 8);
  CHECK(log.block_at(0).kind == BlockKind::text);
  CHECK(log.block_at(1).kind == BlockKind::thinking);
  CHECK(log.block_at(2).kind == BlockKind::tool_call);
  CHECK(log.block_at(3).kind == BlockKind::tool_result);
  CHECK(log.block_at(4).kind == BlockKind::error);
  CHECK(log.block_at(5).kind == BlockKind::warning);
  CHECK(log.block_at(6).kind == BlockKind::user);
  CHECK(log.block_at(7).kind == BlockKind::system);
}

TEST_CASE("append_fragment stores text in block") {
  mistral::tui::ContentLog log;
  log.append_fragment("hello world", content_type::normal);
  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).text == "hello world");
}

// ---------------------------------------------------------------------------
// Same-kind append merges into last block
// ---------------------------------------------------------------------------

TEST_CASE("same-kind append merges into last block") {
  mistral::tui::ContentLog log;
  log.append_fragment("hello ", content_type::normal);
  log.append_fragment("world", content_type::normal);

  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).text == "hello world");
}

TEST_CASE("same-kind append merges for thinking") {
  mistral::tui::ContentLog log;
  log.append_fragment("first ", content_type::thinking);
  log.append_fragment("second", content_type::thinking);

  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).text == "first second");
}

TEST_CASE("same-kind append merges for tool_result") {
  mistral::tui::ContentLog log;
  log.append_fragment("line1\n", content_type::tool_result);
  log.append_fragment("line2\n", content_type::tool_result);

  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).text == "line1\nline2\n");
}

// ---------------------------------------------------------------------------
// Different-kind append creates new block
// ---------------------------------------------------------------------------

TEST_CASE("different-kind append creates new block") {
  mistral::tui::ContentLog log;
  log.append_fragment("hello", content_type::normal);
  log.append_fragment("thinking", content_type::thinking);

  REQUIRE(log.block_count() == 2);
  CHECK(log.block_at(0).text == "hello");
  CHECK(log.block_at(0).kind == BlockKind::text);
  CHECK(log.block_at(1).text == "thinking");
  CHECK(log.block_at(1).kind == BlockKind::thinking);
}

TEST_CASE("alternating types create separate blocks") {
  mistral::tui::ContentLog log;
  log.append_fragment("a", content_type::normal);
  log.append_fragment("b", content_type::thinking);
  log.append_fragment("c", content_type::normal);

  REQUIRE(log.block_count() == 3);
  CHECK(log.block_at(0).text == "a");
  CHECK(log.block_at(1).text == "b");
  CHECK(log.block_at(2).text == "c");
}

TEST_CASE("same kind after gap creates new block") {
  mistral::tui::ContentLog log;
  log.append_fragment("first", content_type::normal);
  log.append_fragment("thinking", content_type::thinking);
  log.append_fragment("third", content_type::normal);

  REQUIRE(log.block_count() == 3);
  CHECK(log.block_at(0).text == "first");
  CHECK(log.block_at(2).text == "third");
  CHECK(log.block_at(2).kind == BlockKind::text);
}

// ---------------------------------------------------------------------------
// flush() updates cached_lines and total_lines
// ---------------------------------------------------------------------------

TEST_CASE("flush updates cached_lines for single block") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;
  log.append_fragment("hello world", content_type::normal);
  log.flush();

  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).cached_lines.size() == 1);
  CHECK(log.block_at(0).cached_lines[0] == "hello world");
  CHECK(log.block_at(0).cached_width == 80);
  CHECK(log.total_lines() == 1);
}

TEST_CASE("flush sets fixed_height to total_lines") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;
  log.append_fragment("hello world", content_type::normal);
  log.flush();

  CHECK(log.fixed_height == log.total_lines());
  CHECK(log.fixed_height == 1);
}

TEST_CASE("flush wraps long text into multiple lines") {
  mistral::tui::ContentLog log;
  log.width = 10;
  log.height = 24;
  log.append_fragment("hello world this is a long line", content_type::normal);
  log.flush();

  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).cached_lines.size() > 1);
  CHECK(log.total_lines() == (int)log.block_at(0).cached_lines.size());
  CHECK(log.fixed_height == log.total_lines());
}

TEST_CASE("flush handles newlines in text") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;
  log.append_fragment("line1\nline2\nline3", content_type::normal);
  log.flush();

  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).cached_lines.size() == 3);
  CHECK(log.block_at(0).cached_lines[0] == "line1");
  CHECK(log.block_at(0).cached_lines[1] == "line2");
  CHECK(log.block_at(0).cached_lines[2] == "line3");
  CHECK(log.total_lines() == 3);
}

TEST_CASE("flush uses 80 as default width when width is 0") {
  mistral::tui::ContentLog log;
  log.width = 0;
  log.height = 24;
  // Two 40-char words separated by a space = 81 chars total.
  // At width 80, the second word won't fit on the first line.
  std::string word(40, 'x');
  std::string long_text = word + " " + word;
  log.append_fragment(long_text, content_type::normal);
  log.flush();

  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).cached_lines.size() == 2);
  CHECK(log.block_at(0).cached_width == 80);
}

// ---------------------------------------------------------------------------
// flush() only re-wraps dirty blocks
// ---------------------------------------------------------------------------

TEST_CASE("flush only re-wraps dirty blocks") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  // First block
  log.append_fragment("first block", content_type::normal);
  log.flush();

  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).cached_lines.size() == 1);
  CHECK(log.block_at(0).dirty == false);

  // Second block (different kind — creates new block)
  log.append_fragment("second block", content_type::thinking);
  log.flush();

  REQUIRE(log.block_count() == 2);
  // First block unchanged
  CHECK(log.block_at(0).cached_lines.size() == 1);
  CHECK(log.block_at(0).cached_lines[0] == "first block");
  // Second block wrapped
  CHECK(log.block_at(1).cached_lines.size() == 1);
  CHECK(log.block_at(1).cached_lines[0] == "second block");
  CHECK(log.total_lines() == 2);
}

TEST_CASE("append after flush re-wraps last block") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("hello", content_type::normal);
  log.flush();
  CHECK(log.total_lines() == 1);
  CHECK(log.block_at(0).cached_lines[0] == "hello");

  // Append more to same block
  log.append_fragment(" world", content_type::normal);
  log.flush();

  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).text == "hello world");
  CHECK(log.block_at(0).cached_lines.size() == 1);
  CHECK(log.block_at(0).cached_lines[0] == "hello world");
  CHECK(log.total_lines() == 1);
}

TEST_CASE("flush with no blocks is no-op") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;
  log.flush();
  CHECK(log.total_lines() == 0);
  CHECK(log.fixed_height == 0);
}

TEST_CASE("flush with no dirty blocks is no-op") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("hello", content_type::normal);
  log.flush();
  CHECK(log.total_lines() == 1);

  // Second flush with no new fragments
  log.flush();
  CHECK(log.total_lines() == 1);
  CHECK(log.fixed_height == 1);
}

TEST_CASE("flush updates total_lines correctly across multiple blocks") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("line1\nline2", content_type::normal);
  log.flush();
  CHECK(log.total_lines() == 2);

  log.append_fragment("thinking1", content_type::thinking);
  log.flush();
  CHECK(log.total_lines() == 3);

  // Append more to first block (same kind as block 0)
  // But since block 1 (thinking) is now last, this creates block 2
  log.append_fragment("more text", content_type::normal);
  log.flush();
  CHECK(log.total_lines() == 4);
}

// ---------------------------------------------------------------------------
// Color and style mapping
// ---------------------------------------------------------------------------

TEST_CASE("text block uses default color") {
  mistral::tui::ContentLog log;
  log.append_fragment("hello", content_type::normal);
  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).color.is_default == true);
  CHECK(log.block_at(0).bold == false);
}

TEST_CASE("thinking block uses input_placeholder color") {
  mistral::tui::ContentLog log;
  log.append_fragment("thinking", content_type::thinking);
  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).color == cpptui::Theme::current().input_placeholder);
  CHECK(log.block_at(0).bold == false);
}

TEST_CASE("tool_call block uses primary color and bold") {
  mistral::tui::ContentLog log;
  log.append_fragment("[tool] read()", content_type::tool_call);
  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).color == cpptui::Theme::current().primary);
  CHECK(log.block_at(0).bold == true);
}

TEST_CASE("tool_result block uses primary color") {
  mistral::tui::ContentLog log;
  log.append_fragment("-> result", content_type::tool_result);
  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).color == cpptui::Theme::current().primary);
  CHECK(log.block_at(0).bold == false);
}

TEST_CASE("error block uses error color and bold") {
  mistral::tui::ContentLog log;
  log.append_fragment("[error] oops", content_type::error);
  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).color == cpptui::Theme::current().error);
  CHECK(log.block_at(0).bold == true);
}

TEST_CASE("warning block uses warning color") {
  mistral::tui::ContentLog log;
  log.append_fragment("[warning] careful", content_type::warning);
  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).color == cpptui::Theme::current().warning);
  CHECK(log.block_at(0).bold == false);
}

TEST_CASE("user block uses success color and bold") {
  mistral::tui::ContentLog log;
  log.append_fragment("> user", content_type::user);
  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).color == cpptui::Theme::current().success);
  CHECK(log.block_at(0).bold == true);
}

TEST_CASE("system block uses warning color") {
  mistral::tui::ContentLog log;
  log.append_fragment("system msg", content_type::system);
  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).color == cpptui::Theme::current().warning);
  CHECK(log.block_at(0).bold == false);
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

TEST_CASE("render produces correct text output") {
  mistral::tui::ContentLog log;
  log.x = 0;
  log.y = 0;
  log.width = 20;
  log.height = 5;

  log.append_fragment("hello", content_type::normal);
  log.flush();

  cpptui::Buffer buffer(20, 5);
  log.render(buffer);

  CHECK(buffer.get(0, 0).content == "h");
  CHECK(buffer.get(1, 0).content == "e");
  CHECK(buffer.get(2, 0).content == "l");
  CHECK(buffer.get(3, 0).content == "l");
  CHECK(buffer.get(4, 0).content == "o");
  // Remaining cells should be spaces
  CHECK(buffer.get(5, 0).content == " ");
}

TEST_CASE("render multiple blocks on separate lines") {
  mistral::tui::ContentLog log;
  log.x = 0;
  log.y = 0;
  log.width = 20;
  log.height = 10;

  log.append_fragment("line1", content_type::normal);
  log.append_fragment("line2", content_type::thinking);
  log.flush();

  cpptui::Buffer buffer(20, 10);
  log.render(buffer);

  // First block on row 0
  CHECK(buffer.get(0, 0).content == "l");
  CHECK(buffer.get(1, 0).content == "i");
  CHECK(buffer.get(2, 0).content == "n");
  CHECK(buffer.get(3, 0).content == "e");
  CHECK(buffer.get(4, 0).content == "1");

  // Second block on row 1
  CHECK(buffer.get(0, 1).content == "l");
  CHECK(buffer.get(1, 1).content == "i");
  CHECK(buffer.get(2, 1).content == "n");
  CHECK(buffer.get(3, 1).content == "e");
  CHECK(buffer.get(4, 1).content == "2");
}

TEST_CASE("render applies error color and bold") {
  mistral::tui::ContentLog log;
  log.x = 0;
  log.y = 0;
  log.width = 20;
  log.height = 5;

  log.append_fragment("error!", content_type::error);
  log.flush();

  cpptui::Buffer buffer(20, 5);
  log.render(buffer);

  CHECK(buffer.get(0, 0).bold == true);
  CHECK(buffer.get(0, 0).fg_color == cpptui::Theme::current().error);
}

TEST_CASE("render applies thinking color") {
  mistral::tui::ContentLog log;
  log.x = 0;
  log.y = 0;
  log.width = 20;
  log.height = 5;

  log.append_fragment("thinking...", content_type::thinking);
  log.flush();

  cpptui::Buffer buffer(20, 5);
  log.render(buffer);

  CHECK(buffer.get(0, 0).fg_color == cpptui::Theme::current().input_placeholder);
  CHECK(buffer.get(0, 0).bold == false);
}

TEST_CASE("render text block uses foreground color") {
  mistral::tui::ContentLog log;
  log.x = 0;
  log.y = 0;
  log.width = 20;
  log.height = 5;

  log.append_fragment("hello", content_type::normal);
  log.flush();

  cpptui::Buffer buffer(20, 5);
  log.render(buffer);

  CHECK(buffer.get(0, 0).fg_color == cpptui::Theme::current().foreground);
  CHECK(buffer.get(0, 0).bold == false);
}

TEST_CASE("render fills remaining width with background") {
  mistral::tui::ContentLog log;
  log.x = 0;
  log.y = 0;
  log.width = 20;
  log.height = 5;

  log.append_fragment("hi", content_type::normal);
  log.flush();

  cpptui::Buffer buffer(20, 5);
  log.render(buffer);

  // Cell after "hi" should be a space with background
  CHECK(buffer.get(2, 0).content == " ");
  CHECK(buffer.get(2, 0).bg_color == cpptui::Theme::current().background);
  // Last cell on the line
  CHECK(buffer.get(19, 0).content == " ");
}

TEST_CASE("render wrapped lines appear on separate rows") {
  mistral::tui::ContentLog log;
  log.x = 0;
  log.y = 0;
  log.width = 5;
  log.height = 10;

  log.append_fragment("hello world", content_type::normal);
  log.flush();

  REQUIRE(log.block_at(0).cached_lines.size() == 2);

  cpptui::Buffer buffer(5, 10);
  log.render(buffer);

  // First wrapped line on row 0
  CHECK(buffer.get(0, 0).content == "h");
  // Second wrapped line on row 1
  CHECK(buffer.get(0, 1).content == "w");
}

TEST_CASE("render with no blocks produces no output") {
  mistral::tui::ContentLog log;
  log.x = 0;
  log.y = 0;
  log.width = 20;
  log.height = 5;

  cpptui::Buffer buffer(20, 5);
  log.render(buffer);

  // Buffer should be all default cells
  CHECK(buffer.get(0, 0).content == " ");
}

TEST_CASE("render with unwrapped blocks skips them") {
  mistral::tui::ContentLog log;
  log.x = 0;
  log.y = 0;
  log.width = 20;
  log.height = 5;

  // Append without flushing — cached_lines is empty
  log.append_fragment("hello", content_type::normal);

  cpptui::Buffer buffer(20, 5);
  log.render(buffer);

  // No content should be rendered
  CHECK(buffer.get(0, 0).content == " ");
}

// ---------------------------------------------------------------------------
// clear()
// ---------------------------------------------------------------------------

TEST_CASE("clear resets all state") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("hello", content_type::normal);
  log.flush();
  CHECK(log.block_count() == 1);
  CHECK(log.total_lines() == 1);

  log.clear();
  CHECK(log.block_count() == 0);
  CHECK(log.total_lines() == 0);
  CHECK(log.fixed_height == 0);
}

TEST_CASE("clear allows appending after clear") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("first", content_type::normal);
  log.flush();
  log.clear();

  log.append_fragment("second", content_type::normal);
  log.flush();

  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).text == "second");
  CHECK(log.total_lines() == 1);
}

TEST_SUITE_END();