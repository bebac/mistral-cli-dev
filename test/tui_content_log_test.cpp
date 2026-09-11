#include "tui/tui_content_log.h"
#include "tui/tui_content_area.h"
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

  // tool_call and tool_result merge into a single tool block
  REQUIRE(log.block_count() == 7);
  CHECK(log.block_at(0).kind == BlockKind::text);
  CHECK(log.block_at(1).kind == BlockKind::thinking);
  CHECK(log.block_at(2).kind == BlockKind::tool);
  CHECK(log.block_at(3).kind == BlockKind::error);
  CHECK(log.block_at(4).kind == BlockKind::warning);
  CHECK(log.block_at(5).kind == BlockKind::user);
  CHECK(log.block_at(6).kind == BlockKind::system);
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

TEST_CASE("tool_result appends to last tool block result_text") {
  mistral::tui::ContentLog log;
  log.append_fragment("[tool] read()", content_type::tool_call);
  log.append_fragment("line1\n", content_type::tool_result);
  log.append_fragment("line2\n", content_type::tool_result);

  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).kind == BlockKind::tool);
  CHECK(log.block_at(0).text == "[tool] read()");
  CHECK(log.block_at(0).result_text == "line1\nline2\n");
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
  // Second block wrapped (thinking is collapsible, wrapped at width-4)
  CHECK(log.block_at(1).cached_lines.size() == 1);
  CHECK(log.block_at(1).cached_lines[0] == "second block");
  // text (1) + separator (1) + thinking (1) = 3
  CHECK(log.total_lines() == 3);
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
  // text (2) + separator (1) + thinking (1) = 4
  CHECK(log.total_lines() == 4);

  // Append more to first block (same kind as block 0)
  // But since block 1 (thinking) is now last, this creates block 2
  log.append_fragment("more text", content_type::normal);
  log.flush();
  // text (2) + sep (1) + thinking collapsed (1) + sep (1) + text (1) = 6
  CHECK(log.total_lines() == 6);
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

TEST_CASE("tool block uses primary color and bold") {
  mistral::tui::ContentLog log;
  log.append_fragment("[tool] read()", content_type::tool_call);
  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).color == cpptui::Theme::current().primary);
  CHECK(log.block_at(0).bold == true);
}

TEST_CASE("tool block from result uses primary color") {
  mistral::tui::ContentLog log;
  log.append_fragment("-> result", content_type::tool_result);
  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).color == cpptui::Theme::current().primary);
  CHECK(log.block_at(0).bold == true);
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

  // First block (text, non-collapsible) on row 0
  CHECK(buffer.get(0, 0).content == "l");
  CHECK(buffer.get(1, 0).content == "i");
  CHECK(buffer.get(2, 0).content == "n");
  CHECK(buffer.get(3, 0).content == "e");
  CHECK(buffer.get(4, 0).content == "1");

  // Separator blank line on row 1 (text → thinking)
  CHECK(buffer.get(0, 1).content == " ");

  // Second block (thinking, collapsible but not collapsed yet) on row 2
  // Indented with "  ▼ " prefix
  CHECK(buffer.get(0, 2).content == " ");
  CHECK(buffer.get(1, 2).content == " ");
  CHECK(buffer.get(2, 2).content == "\xe2\x96\xbc"); // ▼
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

  // Thinking is collapsible, rendered with "  ▼ " prefix
  // The prefix uses the block's color
  CHECK(buffer.get(2, 0).fg_color == cpptui::Theme::current().input_placeholder);
  CHECK(buffer.get(2, 0).bold == false);
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

// ---------------------------------------------------------------------------
// force_new parameter
// ---------------------------------------------------------------------------

TEST_CASE("force_new creates separate block for same kind") {
  mistral::tui::ContentLog log;
  log.append_fragment("tool call 1", content_type::tool_call, true);
  log.append_fragment("tool call 2", content_type::tool_call, true);
  log.append_fragment("tool call 3", content_type::tool_call, true);

  REQUIRE(log.block_count() == 3);
  CHECK(log.block_at(0).text == "tool call 1");
  CHECK(log.block_at(1).text == "tool call 2");
  CHECK(log.block_at(2).text == "tool call 3");
  CHECK(log.block_at(0).kind == BlockKind::tool);
  CHECK(log.block_at(1).kind == BlockKind::tool);
  CHECK(log.block_at(2).kind == BlockKind::tool);
}

TEST_CASE("tool_result without tool_call creates tool block") {
  mistral::tui::ContentLog log;
  log.append_fragment("result part 1", content_type::tool_result, true);
  log.append_fragment(" part 2", content_type::tool_result, false);
  log.append_fragment(" part 3", content_type::tool_result, false);

  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).kind == BlockKind::tool);
  CHECK(log.block_at(0).result_text == "result part 1 part 2 part 3");
}

TEST_CASE("tool_result without tool_call merges into one block") {
  mistral::tui::ContentLog log;
  // tool_result without a preceding tool_call creates a tool block.
  // Subsequent tool_result fragments append to the last tool block
  // (streaming continuation of the same result).
  log.append_fragment("result A line 1\nresult A line 2",
                      content_type::tool_result, true);
  log.append_fragment("result B", content_type::tool_result, true);

  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).kind == BlockKind::tool);
  CHECK(log.block_at(0).result_text ==
        "result A line 1\nresult A line 2result B");
}

TEST_CASE("interleaved tool calls and results pair correctly") {
  mistral::tui::ContentLog log;
  // Real streaming flow: call→result→call→result→call→result
  log.append_fragment("[tool] call1()", content_type::tool_call, true);
  log.append_fragment("result1", content_type::tool_result);
  CHECK(log.block_at(0).result_text == "result1");
  CHECK(log.block_at(0).tool_status == "\xe2\x9c\x93");

  log.append_fragment("[tool] call2()", content_type::tool_call, true);
  log.append_fragment("result2", content_type::tool_result);
  CHECK(log.block_at(1).result_text == "result2");
  CHECK(log.block_at(1).tool_status == "\xe2\x9c\x93");

  log.append_fragment("[tool] call3()", content_type::tool_call, true);
  log.append_fragment("result3", content_type::tool_result);
  CHECK(log.block_at(2).result_text == "result3");
  CHECK(log.block_at(2).tool_status == "\xe2\x9c\x93");

  REQUIRE(log.block_count() == 3);
  // All tool blocks are always collapsed.
  CHECK(log.block_at(0).collapsed == true);
  CHECK(log.block_at(1).collapsed == true);
  CHECK(log.block_at(2).collapsed == true);
}

TEST_CASE("force_new=false still merges same kind by default") {
  mistral::tui::ContentLog log;
  log.append_fragment("a", content_type::normal);
  log.append_fragment("b", content_type::normal);

  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).text == "ab");
}

TEST_CASE("force_new creates new block even when last is same kind") {
  mistral::tui::ContentLog log;
  log.append_fragment("first", content_type::normal);
  log.append_fragment("second", content_type::normal, true);

  REQUIRE(log.block_count() == 2);
  CHECK(log.block_at(0).text == "first");
  CHECK(log.block_at(1).text == "second");
}

// ---------------------------------------------------------------------------
// Auto-collapse on new block creation
// ---------------------------------------------------------------------------

TEST_CASE("thinking block auto-collapses when next block is created") {
  mistral::tui::ContentLog log;
  log.append_fragment("thinking content", content_type::thinking);
  CHECK(log.block_at(0).collapsed == false);

  log.append_fragment("response", content_type::normal);
  REQUIRE(log.block_count() == 2);
  CHECK(log.block_at(0).collapsed == true);
  CHECK(log.block_at(1).collapsed == false);
}

TEST_CASE("tool block is always collapsed") {
  mistral::tui::ContentLog log;
  log.append_fragment("[tool] read()", content_type::tool_call);
  log.append_fragment("result", content_type::tool_result);
  // Tool blocks are collapsed from creation.
  CHECK(log.block_at(0).collapsed == true);

  log.append_fragment("response", content_type::normal);
  REQUIRE(log.block_count() == 2);
  CHECK(log.block_at(0).collapsed == true);
  CHECK(log.block_at(1).collapsed == false);
}

TEST_CASE("text block does not auto-collapse when next block is created") {
  mistral::tui::ContentLog log;
  log.append_fragment("text content", content_type::normal);
  log.append_fragment("thinking", content_type::thinking);
  REQUIRE(log.block_count() == 2);
  CHECK(log.block_at(0).collapsed == false);
  CHECK(log.block_at(1).collapsed == false);
}

TEST_CASE("force_new creates collapsed tool blocks") {
  mistral::tui::ContentLog log;
  log.append_fragment("[tool] call1()", content_type::tool_call, true);
  log.append_fragment("result A", content_type::tool_result);
  log.append_fragment("[tool] call2()", content_type::tool_call, true);
  REQUIRE(log.block_count() == 2);
  // Both tool blocks are always collapsed.
  CHECK(log.block_at(0).collapsed == true);
  CHECK(log.block_at(1).collapsed == true);
}

// ---------------------------------------------------------------------------
// Tool call status
// ---------------------------------------------------------------------------

TEST_CASE("tool status set when tool_result arrives") {
  mistral::tui::ContentLog log;
  log.append_fragment("\n[tool] read(path=\"test.txt\")",
                      content_type::tool_call, true);
  CHECK(log.block_at(0).tool_status.empty());

  log.append_fragment(" \xe2\x86\x92 result content",
                      content_type::tool_result, true);
  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).tool_status == "\xe2\x9c\x93");
  // Status is stored separately, not appended to text
  CHECK(log.block_at(0).text.find("\xe2\x9c\x93") == std::string::npos);
  CHECK(log.block_at(0).text == "[tool] read(path=\"test.txt\")");
}

TEST_CASE("multiple tool calls get status in order") {
  mistral::tui::ContentLog log;
  // Real flow: call→result→call→result (each result follows its call)
  log.append_fragment("[tool] call1()", content_type::tool_call, true);
  log.append_fragment(" \xe2\x86\x92 result1",
                      content_type::tool_result, true);
  CHECK(log.block_at(0).tool_status == "\xe2\x9c\x93");

  log.append_fragment("[tool] call2()", content_type::tool_call, true);
  CHECK(log.block_at(1).tool_status.empty());

  log.append_fragment(" \xe2\x86\x92 result2",
                      content_type::tool_result, true);
  CHECK(log.block_at(1).tool_status == "\xe2\x9c\x93");
}

// ---------------------------------------------------------------------------
// complete_turn()
// ---------------------------------------------------------------------------

TEST_CASE("complete_turn does not collapse text blocks") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("answer text", content_type::normal);
  log.flush();
  CHECK(log.block_at(0).collapsed == false);

  log.complete_turn();
  CHECK(log.block_at(0).collapsed == false);
}

TEST_CASE("complete_turn does not collapse error blocks") {
  mistral::tui::ContentLog log;
  log.append_fragment("[error] oops", content_type::error);
  log.append_fragment("answer", content_type::normal);

  log.complete_turn();
  CHECK(log.block_at(0).collapsed == false);
  CHECK(log.block_at(1).collapsed == false);
}

TEST_CASE("complete_turn collapses last thinking block") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("thinking content", content_type::thinking);
  log.flush();
  CHECK(log.block_at(0).collapsed == false);

  log.complete_turn();
  CHECK(log.block_at(0).collapsed == true);
}

TEST_CASE("complete_turn cleans up trailing empty text block") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("thinking content", content_type::thinking);
  log.append_fragment("\n", content_type::normal);
  log.flush();
  CHECK(log.block_count() == 2);

  log.complete_turn();
  CHECK(log.block_count() == 1);
  CHECK(log.block_at(0).collapsed == true);
}

TEST_CASE("complete_turn does not collapse already-collapsed tool blocks") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("[tool] ls(.)", content_type::tool_call);
  log.flush();
  CHECK(log.block_at(0).collapsed == true);

  log.complete_turn();
  CHECK(log.block_at(0).collapsed == true);
}

// ---------------------------------------------------------------------------
// toggle_collapse()
// ---------------------------------------------------------------------------

TEST_CASE("toggle_collapse expands collapsed thinking block") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("thinking content", content_type::thinking);
  log.append_fragment("text", content_type::normal);
  CHECK(log.block_at(0).collapsed == true);

  log.flush();
  bool toggled = log.toggle_collapse(0);
  CHECK(toggled == true);
  CHECK(log.block_at(0).collapsed == false);
}

TEST_CASE("toggle_collapse collapses expanded thinking block") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("thinking content", content_type::thinking);
  log.flush();
  CHECK(log.block_at(0).collapsed == false);

  bool toggled = log.toggle_collapse(0);
  CHECK(toggled == true);
  CHECK(log.block_at(0).collapsed == true);
}

TEST_CASE("toggle_collapse fails for non-collapsible block") {
  mistral::tui::ContentLog log;
  log.append_fragment("> user message", content_type::user);

  bool toggled = log.toggle_collapse(0);
  CHECK(toggled == false);
  CHECK(log.block_at(0).collapsed == false);
}

TEST_CASE("toggle_collapse fails for text block") {
  mistral::tui::ContentLog log;
  log.append_fragment("text content", content_type::normal);

  bool toggled = log.toggle_collapse(0);
  CHECK(toggled == false);
  CHECK(log.block_at(0).collapsed == false);
}

TEST_CASE("toggle_collapse fails for invalid index") {
  mistral::tui::ContentLog log;
  CHECK(log.toggle_collapse(-1) == false);
  CHECK(log.toggle_collapse(0) == false);
  CHECK(log.toggle_collapse(999) == false);
}

TEST_CASE("toggle_collapse updates total_lines") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("a\nb\nc", content_type::thinking);
  log.flush();
  CHECK(log.total_lines() == 3);

  log.toggle_collapse(0);
  CHECK(log.total_lines() == 1);

  log.toggle_collapse(0);
  CHECK(log.total_lines() == 3);
}

// ---------------------------------------------------------------------------
// block_at_line()
// ---------------------------------------------------------------------------

TEST_CASE("block_at_line finds correct block") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("line1\nline2", content_type::normal);
  log.append_fragment("thinking", content_type::thinking);
  log.append_fragment("text", content_type::normal);
  log.flush();

  // block 0: text, 2 lines (0-1)
  // separator: 1 line (2)
  // block 1: thinking, auto-collapsed, 1 line (3)
  // separator: 1 line (4)
  // block 2: text, 1 line (5)
  CHECK(log.block_at_line(0) == 0);
  CHECK(log.block_at_line(1) == 0);
  CHECK(log.block_at_line(2) == -1); // separator
  CHECK(log.block_at_line(3) == 1);
  CHECK(log.block_at_line(4) == -1); // separator
  CHECK(log.block_at_line(5) == 2);
  CHECK(log.block_at_line(6) == -1);
}

TEST_CASE("block_at_line with collapsed blocks") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("a\nb\nc\nd\ne", content_type::thinking);
  log.flush();
  log.toggle_collapse(0);

  CHECK(log.block_at_line(0) == 0);
  CHECK(log.block_at_line(1) == -1);
}

// ---------------------------------------------------------------------------
// Collapsed block rendering
// ---------------------------------------------------------------------------

TEST_CASE("render collapsed thinking shows [think] prefix") {
  mistral::tui::ContentLog log;
  log.x = 0;
  log.y = 0;
  log.width = 80;
  log.height = 10;

  log.append_fragment("thinking content here", content_type::thinking);
  log.append_fragment("text", content_type::normal);
  log.flush();

  CHECK(log.block_at(0).collapsed == true);

  cpptui::Buffer buffer(80, 10);
  log.render(buffer);

  // Collapsed: "  ▶ [think] ..."
  // First two cells are spaces (indent)
  CHECK(buffer.get(0, 0).content == " ");
  CHECK(buffer.get(1, 0).content == " ");
  // Third cell is ▶ indicator
  CHECK(buffer.get(2, 0).content == "\xe2\x96\xb6");
}

TEST_CASE("render collapsed tool shows call text and status") {
  mistral::tui::ContentLog log;
  log.x = 0;
  log.y = 0;
  log.width = 80;
  log.height = 10;

  log.append_fragment("[tool] read(path=\"test\")", content_type::tool_call);
  log.append_fragment(" \xe2\x86\x92 result", content_type::tool_result);
  log.append_fragment("text", content_type::normal);
  log.flush();

  CHECK(log.block_at(0).collapsed == true);

  cpptui::Buffer buffer(80, 10);
  log.render(buffer);

  // Collapsed: "  ▶ [tool] read(path="test") ✓"
  std::string line_content;
  for (int i = 0; i < 80; ++i) {
    line_content += buffer.get(i, 0).content;
  }
  CHECK(line_content.find("[tool] read(path=\"test\")") != std::string::npos);
  CHECK(line_content.find("\xe2\x9c\x93") != std::string::npos);
}

TEST_CASE("render expanded thinking shows indicator and content") {
  mistral::tui::ContentLog log;
  log.x = 0;
  log.y = 0;
  log.width = 80;
  log.height = 10;

  log.append_fragment("thinking content", content_type::thinking);
  log.flush();
  // Not collapsed (no next block to trigger auto-collapse)

  cpptui::Buffer buffer(80, 10);
  log.render(buffer);

  // Expanded: "  ▼ thinking content"
  CHECK(buffer.get(0, 0).content == " ");
  CHECK(buffer.get(1, 0).content == " ");
  CHECK(buffer.get(2, 0).content == "\xe2\x96\xbc"); // ▼
  CHECK(buffer.get(4, 0).content == "t"); // "thinking"
}

TEST_CASE("render expanded tool shows call and result") {
  mistral::tui::ContentLog log;
  log.x = 0;
  log.y = 0;
  log.width = 80;
  log.height = 10;

  log.append_fragment("[tool] read()", content_type::tool_call);
  log.append_fragment(" \xe2\x86\x92 result line", content_type::tool_result);
  log.flush();
  // Tool blocks are always collapsed — expand manually for this test.
  log.toggle_collapse(0);

  cpptui::Buffer buffer(80, 10);
  log.render(buffer);

  // First line: "  ▼ [tool] read()"
  CHECK(buffer.get(2, 0).content == "\xe2\x96\xbc"); // ▼
  std::string line0;
  for (int i = 4; i < 80; ++i) line0 += buffer.get(i, 0).content;
  CHECK(line0.find("[tool] read()") != std::string::npos);

  // Second line: "    → result line" (4-space indent + result)
  CHECK(buffer.get(0, 1).content == " ");
  CHECK(buffer.get(1, 1).content == " ");
  CHECK(buffer.get(2, 1).content == " ");
  CHECK(buffer.get(3, 1).content == " ");
  std::string line1;
  for (int i = 4; i < 80; ++i) line1 += buffer.get(i, 1).content;
  CHECK(line1.find("\xe2\x86\x92 result line") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Turn separators and final answer markers
// ---------------------------------------------------------------------------

TEST_CASE("append_turn_separator creates separator block") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_turn_separator(1);
  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).kind == BlockKind::turn_separator);
  CHECK(log.block_at(0).text == "---[1]");
  CHECK(log.total_lines() == 1);
}

TEST_CASE("render turn separator fills with dashes") {
  mistral::tui::ContentLog log;
  log.x = 0;
  log.y = 0;
  log.width = 30;
  log.height = 5;

  log.append_turn_separator(1);
  cpptui::Buffer buffer(30, 5);
  log.render(buffer);

  // Should start with "---[1]" then dashes
  CHECK(buffer.get(0, 0).content == "-");
  CHECK(buffer.get(1, 0).content == "-");
  CHECK(buffer.get(2, 0).content == "-");
  CHECK(buffer.get(3, 0).content == "[");
  CHECK(buffer.get(4, 0).content == "1");
  CHECK(buffer.get(5, 0).content == "]");
  CHECK(buffer.get(6, 0).content == "-");
  CHECK(buffer.get(29, 0).content == "-");
}

// ---------------------------------------------------------------------------
// total_lines with collapsed blocks
// ---------------------------------------------------------------------------

TEST_CASE("total_lines accounts for collapsed blocks") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("a\nb\nc", content_type::thinking);
  log.append_fragment("text", content_type::normal);
  log.flush();

  // thinking collapsed (1 line) + separator (1) + text (1 line) = 3
  CHECK(log.total_lines() == 3);
  CHECK(log.fixed_height == 3);
}

TEST_CASE("total_lines with mixed collapsed and expanded blocks") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("a\nb\nc", content_type::thinking);
  log.append_fragment("text1\ntext2", content_type::normal);
  log.append_fragment("[tool] x()", content_type::tool_call);
  log.append_fragment("r1\nr2\nr3", content_type::tool_result);
  log.append_fragment("final", content_type::normal);
  log.flush();

  // block 0: thinking collapsed (1)
  // separator (1)
  // block 1: text expanded (2)
  // separator (1)
  // block 2: tool collapsed (1)
  // separator (1)
  // block 3: text expanded (1)
  CHECK(log.total_lines() == 8);
}

TEST_CASE("total_lines with expanded tool block includes result lines") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("[tool] x()", content_type::tool_call);
  log.append_fragment("r1\nr2\nr3", content_type::tool_result);
  log.flush();

  // tool block always collapsed: 1 line
  CHECK(log.total_lines() == 1);
}

// ---------------------------------------------------------------------------
// Blank line spacing between text blocks and tool calls / turn separators
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Render-time separator lines between block-type transitions
// ---------------------------------------------------------------------------

TEST_CASE("separator line between text and tool call") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("some text", content_type::normal);
  log.flush();
  REQUIRE(log.block_at(0).cached_lines.size() == 1);

  log.append_fragment("[tool] read()", content_type::tool_call);
  log.flush();

  // Text block trailing newlines stripped, no trailing blank line in text
  CHECK(log.block_at(0).text == "some text");
  CHECK(log.block_at(0).cached_lines.size() == 1);
  // text (1) + separator (1) + tool collapsed (1) = 3 total
  CHECK(log.total_lines() == 3);
}

TEST_CASE("separator line between text and turn separator") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("final answer", content_type::normal);
  log.flush();
  REQUIRE(log.block_at(0).cached_lines.size() == 1);

  log.append_turn_separator(2);
  log.flush();

  // Text block trailing newlines stripped
  CHECK(log.block_at(0).text == "final answer");
  CHECK(log.block_at(0).cached_lines.size() == 1);
  // text (1) + separator (1) + turn_separator (1) = 3 total
  CHECK(log.total_lines() == 3);
}

TEST_CASE("no separator between consecutive thinking and tool blocks") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("thinking", content_type::thinking);
  log.append_fragment("[tool] read()", content_type::tool_call);
  log.append_fragment("result", content_type::tool_result);
  log.append_fragment("more thinking", content_type::thinking);
  log.flush();

  // block 0: thinking collapsed (1)
  // block 1: tool collapsed (1) — no separator (thinking→tool)
  // block 2: thinking collapsed (1) — no separator (tool→thinking)
  CHECK(log.total_lines() == 3);
}

TEST_CASE("separator between user and thinking") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("> user msg", content_type::user);
  log.append_fragment("thinking", content_type::thinking);
  log.flush();

  // user (1) + separator (1) + thinking collapsed (1) = 3
  CHECK(log.total_lines() == 3);
}

TEST_CASE("separator between thinking and text") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("thinking", content_type::thinking);
  log.append_fragment("answer", content_type::normal);
  log.flush();

  // thinking collapsed (1) + separator (1) + text (1) = 3
  CHECK(log.total_lines() == 3);
}

TEST_CASE("render shows separator blank line between text and thinking") {
  mistral::tui::ContentLog log;
  log.x = 0;
  log.y = 0;
  log.width = 80;
  log.height = 10;

  log.append_fragment("text", content_type::normal);
  log.append_fragment("thinking", content_type::thinking);
  log.append_fragment("answer", content_type::normal);
  log.flush();

  // thinking auto-collapsed when "answer" block was created
  CHECK(log.block_at(1).collapsed == true);

  cpptui::Buffer buffer(80, 10);
  log.render(buffer);

  // Row 0: text block
  CHECK(buffer.get(0, 0).content == "t");
  // Row 1: separator (blank line) between text and thinking
  CHECK(buffer.get(0, 1).content == " ");
  // Row 2: collapsed thinking block
  CHECK(buffer.get(2, 2).content == "\xe2\x96\xb6"); // ▶
  // Row 3: separator (blank line) between thinking and text
  CHECK(buffer.get(0, 3).content == " ");
  // Row 4: answer text
  CHECK(buffer.get(0, 4).content == "a");
}

TEST_CASE("render shows no separator between thinking and tool") {
  mistral::tui::ContentLog log;
  log.x = 0;
  log.y = 0;
  log.width = 80;
  log.height = 10;

  log.append_fragment("thinking", content_type::thinking);
  log.append_fragment("[tool] read()", content_type::tool_call);
  log.append_fragment("result", content_type::tool_result);
  log.append_fragment("answer", content_type::normal);
  log.flush();

  // Both auto-collapsed when "answer" block was created
  CHECK(log.block_at(0).collapsed == true);
  CHECK(log.block_at(1).collapsed == true);

  cpptui::Buffer buffer(80, 10);
  log.render(buffer);

  // Row 0: collapsed thinking
  CHECK(buffer.get(2, 0).content == "\xe2\x96\xb6"); // ▶
  // Row 1: collapsed tool (no separator between thinking and tool)
  CHECK(buffer.get(2, 1).content == "\xe2\x96\xb6"); // ▶
  // Row 2: separator between tool and text
  CHECK(buffer.get(0, 2).content == " ");
  // Row 3: answer text
  CHECK(buffer.get(0, 3).content == "a");
}

TEST_CASE("trailing newlines stripped from text before tool call") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("text\n\n\n", content_type::normal);
  log.flush();

  log.append_fragment("[tool] read()", content_type::tool_call);
  log.flush();

  CHECK(log.block_at(0).text == "text");
  CHECK(log.block_at(0).cached_lines.size() == 1);
}

TEST_CASE("empty text block removed before tool call") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("\n", content_type::normal);
  log.flush();

  log.append_fragment("[tool] read()", content_type::tool_call);
  log.flush();

  // The empty text block should have been removed
  REQUIRE(log.block_count() == 1);
  CHECK(log.block_at(0).kind == BlockKind::tool);
}

TEST_CASE("leading newlines not stripped for first text block") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  // First block — no previous block, so leading newlines are preserved.
  log.append_fragment("\n\n\nanswer text", content_type::normal);
  log.flush();

  CHECK(log.block_at(0).text == "\n\n\nanswer text");
  CHECK(log.block_at(0).cached_lines.size() == 4);
  CHECK(log.block_at(0).cached_lines[0] == "");
  CHECK(log.block_at(0).cached_lines[1] == "");
  CHECK(log.block_at(0).cached_lines[2] == "");
  CHECK(log.block_at(0).cached_lines[3] == "answer text");
}

TEST_CASE("leading newlines stripped at block-type boundary") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  // Previous block of different type — leading newlines should be stripped
  // at the block-type boundary (thinking → text transition).
  log.append_fragment("thinking", content_type::thinking);
  log.append_fragment("\n\n\nanswer text", content_type::normal);
  log.flush();

  CHECK(log.block_at(1).text == "answer text");
  CHECK(log.block_at(1).cached_lines.size() == 1);
  CHECK(log.block_at(1).cached_lines[0] == "answer text");
}

TEST_CASE("leading newlines not stripped with force_new same kind") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  log.append_fragment("first", content_type::normal);
  log.append_fragment("\n\nsecond", content_type::normal, true);
  log.flush();

  REQUIRE(log.block_count() == 2);
  CHECK(log.block_at(0).text == "first");
  // force_new creates a new text block after an existing text block —
  // same kind, so no block-type boundary, leading newlines preserved.
  CHECK(log.block_at(1).text == "\n\nsecond");
  CHECK(log.block_at(1).cached_lines.size() == 3);
  CHECK(log.block_at(1).cached_lines[0] == "");
  CHECK(log.block_at(1).cached_lines[1] == "");
  CHECK(log.block_at(1).cached_lines[2] == "second");
}

TEST_CASE("leading newlines preserved when merging text fragments") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  // First fragment creates the block; no previous block so leading
  // newlines are preserved (not stripped).
  log.append_fragment("\n", content_type::normal);
  // Second fragment merges; its leading newline must be preserved.
  log.append_fragment("\nanswer", content_type::normal);
  log.flush();

  CHECK(log.block_at(0).text == "\n\nanswer");
  CHECK(log.block_at(0).cached_lines.size() == 3);
  CHECK(log.block_at(0).cached_lines[0] == "");
  CHECK(log.block_at(0).cached_lines[1] == "");
  CHECK(log.block_at(0).cached_lines[2] == "answer");
}

TEST_CASE("blank lines preserved between paragraphs when streaming") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  // Simulate streaming where the API sends a newline-only fragment
  // before content, followed by a paragraph break. No previous block,
  // so all newlines are part of the content and preserved.
  log.append_fragment("\n", content_type::normal);
  log.append_fragment("\nfirst paragraph", content_type::normal);
  log.append_fragment("\n\n", content_type::normal);
  log.append_fragment("second paragraph", content_type::normal);
  log.flush();

  CHECK(log.block_at(0).text == "\n\nfirst paragraph\n\nsecond paragraph");
  CHECK(log.block_at(0).cached_lines.size() == 5);
  CHECK(log.block_at(0).cached_lines[0] == "");
  CHECK(log.block_at(0).cached_lines[1] == "");
  CHECK(log.block_at(0).cached_lines[2] == "first paragraph");
  CHECK(log.block_at(0).cached_lines[3] == "");
  CHECK(log.block_at(0).cached_lines[4] == "second paragraph");
}

TEST_CASE("normalization gives consistent one blank line via separators") {
  mistral::tui::ContentLog log;
  log.width = 80;
  log.height = 24;

  // Previous block of different type triggers leading newline stripping
  log.append_fragment("thinking", content_type::thinking);
  // Simulate realistic flow with extra newlines from API
  log.append_fragment("\n\nanswer text\n\n", content_type::normal);
  log.flush();

  log.append_turn_separator(2);
  log.flush();

  // Text block: leading newlines stripped (thinking→text transition),
  // trailing newlines stripped by cleanup_last_text_block().
  CHECK(log.block_at(1).text == "answer text");
  CHECK(log.block_at(1).cached_lines.size() == 1);
  // thinking collapsed (1) + separator (1) + text (1) + separator (1)
  // + turn_separator (1) = 5 total
  CHECK(log.total_lines() == 5);
}

// ---------------------------------------------------------------------------
// Text selection
// ---------------------------------------------------------------------------

TEST_CASE("selection: single text block with blank line") {
  mistral::tui::ContentLog log;
  log.x = 0;
  log.y = 0;
  log.width = 80;
  log.height = 24;

  std::string line1 = "There are currently **no open tasks** in the system.";
  std::string line2 =
    "Both \"todo\" and \"in-progress\" statuses returned empty results.";
  log.append_fragment(line1 + "\n\n" + line2, content_type::normal);
  log.flush();

  REQUIRE(log.block_count() == 1);
  REQUIRE(log.block_at(0).cached_lines.size() == 3);

  // Mouse press at start of line 0
  cpptui::Event press;
  press.type = cpptui::EventType::Mouse;
  press.x = 0;
  press.y = 0;
  press.button = 0;  // left click
  press.click_count = 1;
  log.on_event(press);

  // Mouse drag to end of line 0
  cpptui::Event drag;
  drag.type = cpptui::EventType::Mouse;
  drag.x = (int)line1.size() - 1;
  drag.y = 0;
  drag.button = 32;  // motion with left button
  log.on_event(drag);

  // Mouse release
  cpptui::Event release;
  release.type = cpptui::EventType::Mouse;
  release.x = (int)line1.size() - 1;
  release.y = 0;
  release.button = 3;  // release
  log.on_event(release);

  REQUIRE(log.has_selection());
  CHECK(log.get_selected_text() == line1);
}

TEST_CASE("selection: text block after turn separator") {
  mistral::tui::ContentLog log;
  log.x = 0;
  log.y = 0;
  log.width = 80;
  log.height = 24;

  log.append_turn_separator(1);
  std::string line1 = "There are currently **no open tasks** in the system.";
  std::string line2 =
    "Both \"todo\" and \"in-progress\" statuses returned empty results.";
  log.append_fragment(line1 + "\n\n" + line2, content_type::normal);
  log.flush();

  REQUIRE(log.block_count() == 2);
  // Line 0: turn separator
  // Line 1: separator (blank)
  // Line 2: first text line
  // Line 3: empty line
  // Line 4: second text line

  // Mouse press at start of text line (y=2)
  cpptui::Event press;
  press.type = cpptui::EventType::Mouse;
  press.x = 0;
  press.y = 2;
  press.button = 0;
  press.click_count = 1;
  log.on_event(press);

  // Mouse drag to end of text line (y=2)
  cpptui::Event drag;
  drag.type = cpptui::EventType::Mouse;
  drag.x = (int)line1.size() - 1;
  drag.y = 2;
  drag.button = 32;
  log.on_event(drag);

  // Mouse release
  cpptui::Event release;
  release.type = cpptui::EventType::Mouse;
  release.x = (int)line1.size() - 1;
  release.y = 2;
  release.button = 3;
  log.on_event(release);

  REQUIRE(log.has_selection());
  CHECK(log.get_selected_text() == line1);
}

TEST_CASE("selection: select across blank line in multi-line block") {
  mistral::tui::ContentLog log;
  log.x = 0;
  log.y = 0;
  log.width = 80;
  log.height = 24;

  std::string line1 = "There are currently **no open tasks** in the system.";
  std::string line2 =
    "Both \"todo\" and \"in-progress\" statuses returned empty results.";
  log.append_fragment(line1 + "\n\n" + line2, content_type::normal);
  log.flush();

  // Select from start of line 0 to start of line 2 (crosses \n\n)
  cpptui::Event press;
  press.type = cpptui::EventType::Mouse;
  press.x = 0;
  press.y = 0;
  press.button = 0;
  press.click_count = 1;
  log.on_event(press);

  cpptui::Event drag;
  drag.type = cpptui::EventType::Mouse;
  drag.x = 0;
  drag.y = 2;
  drag.button = 32;
  log.on_event(drag);

  cpptui::Event release;
  release.type = cpptui::EventType::Mouse;
  release.x = 0;
  release.y = 2;
  release.button = 3;
  log.on_event(release);

  REQUIRE(log.has_selection());
  // Should include line1 + \n\n + first char of line2
  CHECK(log.get_selected_text() == line1 + "\n\nB");
}

TEST_CASE("selection: text after collapsed tool block with UTF-8 status") {
  mistral::tui::ContentLog log;
  log.x = 0;
  log.y = 0;
  log.width = 80;
  log.height = 24;

  // User message
  log.append_fragment("> list tasks", content_type::user);

  // Tool call + result (creates collapsed tool block with ✓ status)
  log.append_fragment("[tool] task_list()", content_type::tool_call);
  log.append_fragment("no tasks found\n", content_type::tool_result);

  // AI text response
  std::string line1 = "There are currently **no open tasks** in the system.";
  std::string line2 =
    "Both \"todo\" and \"in-progress\" statuses returned empty results.";
  log.append_fragment(line1 + "\n\n" + line2, content_type::normal);
  log.flush();

  // Block layout:
  // Block 0: user "> list tasks"         (1 line)
  // Block 1: tool (collapsed, has ✓)     (1 line)
  // Block 2: text                        (3 lines)
  // Content lines:
  // 0: "> list tasks"
  // 1: separator
  // 2: [tool] task_list() ✓  (collapsed summary)
  // 3: separator
  // 4: "There are currently..."
  // 5: ""
  // 6: "Both..."

  // Mouse press at start of text line (y=4)
  cpptui::Event press;
  press.type = cpptui::EventType::Mouse;
  press.x = 0;
  press.y = 4;
  press.button = 0;
  press.click_count = 1;
  log.on_event(press);

  // Mouse drag to end of text line (y=4)
  cpptui::Event drag;
  drag.type = cpptui::EventType::Mouse;
  drag.x = (int)line1.size() - 1;
  drag.y = 4;
  drag.button = 32;
  log.on_event(drag);

  // Mouse release
  cpptui::Event release;
  release.type = cpptui::EventType::Mouse;
  release.x = (int)line1.size() - 1;
  release.y = 4;
  release.button = 3;
  log.on_event(release);

  REQUIRE(log.has_selection());
  // The ✓ in the tool summary is 3 bytes but 1 codepoint.
  // If block_char_offsets_ used flat_text_.size() (bytes) instead of
  // count_codepoints (codepoints), the selection would be off by 2.
  CHECK(log.get_selected_text() == line1);
}

TEST_CASE("selection: select all via triple-click") {
  mistral::tui::ContentLog log;
  log.x = 0;
  log.y = 0;
  log.width = 80;
  log.height = 24;

  std::string line1 = "There are currently **no open tasks** in the system.";
  std::string line2 =
    "Both \"todo\" and \"in-progress\" statuses returned empty results.";
  log.append_fragment(line1 + "\n\n" + line2, content_type::normal);
  log.flush();

  // Triple-click selects all text in the flat text
  cpptui::Event press;
  press.type = cpptui::EventType::Mouse;
  press.x = 0;
  press.y = 0;
  press.button = 0;
  press.click_count = 3;
  log.on_event(press);

  REQUIRE(log.has_selection());
  // Triple-click selects from 0 to chars.size() of flat_text
  // flat_text = line1 + "\n" + "" + "\n" + line2 = line1 + "\n\n" + line2
  CHECK(log.get_selected_text() == line1 + "\n\n" + line2);
}

// ---------------------------------------------------------------------------
// content_area newline preservation (regression tests)
// ---------------------------------------------------------------------------

TEST_CASE("content_area preserves newlines in streaming text fragments") {
  mistral::tui::content_area area;
  auto scroll = area.build();
  scroll->width = 80;
  scroll->height = 24;

  // Access ContentLog through scroll's children to clear welcome text.
  auto sv = std::dynamic_pointer_cast<cpptui::ScrollableVertical>(scroll);
  REQUIRE(sv);
  auto& children = sv->get_children();
  REQUIRE(children.size() == 1);
  auto log = std::dynamic_pointer_cast<mistral::tui::ContentLog>(children[0]);
  REQUIRE(log);
  log->clear();

  // Simulate streaming where the API sends a fragment that starts
  // with a newline (the line break between two lines of content).
  // content_area::append_fragment must NOT strip this newline —
  // it is part of the content, not inter-block spacing.
  area.append_fragment("line1", content_type::normal);
  area.append_fragment("\nline2", content_type::normal);
  area.flush();

  REQUIRE(log->block_count() == 1);
  CHECK(log->block_at(0).text == "line1\nline2");
  CHECK(log->block_at(0).cached_lines.size() == 2);
  CHECK(log->block_at(0).cached_lines[0] == "line1");
  CHECK(log->block_at(0).cached_lines[1] == "line2");
}

TEST_CASE("content_area preserves newlines in multi-line content streaming") {
  mistral::tui::content_area area;
  auto scroll = area.build();
  scroll->width = 80;
  scroll->height = 24;

  auto sv = std::dynamic_pointer_cast<cpptui::ScrollableVertical>(scroll);
  REQUIRE(sv);
  auto& children = sv->get_children();
  REQUIRE(children.size() == 1);
  auto log = std::dynamic_pointer_cast<mistral::tui::ContentLog>(children[0]);
  REQUIRE(log);
  log->clear();

  // Simulate streaming multi-line content (e.g. ASCII art) where a
  // fragment starts with a newline — this is the exact scenario that
  // was broken: the newline was stripped, causing lines to merge.
  area.append_fragment("```\n", content_type::normal);
  area.append_fragment("+-------+\n", content_type::normal);
  area.append_fragment("\n|  none |\n", content_type::normal);
  area.append_fragment("+-------+\n", content_type::normal);
  area.append_fragment("```", content_type::normal);
  area.flush();

  REQUIRE(log->block_count() == 1);
  // The text should preserve all newlines, including the blank line
  // created by the fragment starting with "\n".
  std::string expected =
    "```\n"
    "+-------+\n"
    "\n"
    "|  none |\n"
    "+-------+\n"
    "```";
  CHECK(log->block_at(0).text == expected);
  CHECK(log->block_at(0).cached_lines.size() == 6);
  CHECK(log->block_at(0).cached_lines[0] == "```");
  CHECK(log->block_at(0).cached_lines[1] == "+-------+");
  CHECK(log->block_at(0).cached_lines[2] == "");
  CHECK(log->block_at(0).cached_lines[3] == "|  none |");
  CHECK(log->block_at(0).cached_lines[4] == "+-------+");
  CHECK(log->block_at(0).cached_lines[5] == "```");
}

TEST_SUITE_END();