#include "cpptui.hpp"
#include "pty_test_harness.h"
#include <doctest/doctest.h>

#include <atomic>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// Regression tests for standalone ESC key handling in cpp-tui's VTParser.
//
// The VTParser enters Escape state on byte 27 and waits for a follow-up byte
// to distinguish escape sequences (CSI, SS3) and SGR mouse reports from
// Alt+key. Two bugs are covered here:
//
//   1. A lone ESC (no follow-up byte within the input grace period) must be
//      delivered as a standalone key event (key=27), not silently discarded.
//      Fixed in the fork by VTParser::timeout() and upstream by
//      VTParser::flush() (jonoton/cpp-tui 3ab1f319).
//
//   2. An ESC immediately followed by another escape sequence (e.g. an SGR
//      mouse report arriving right after the user pressed ESC) must emit the
//      first ESC as a standalone key event and parse the following sequence
//      normally — not misparse it as Alt+Escape. Fixed upstream in
//      jonoton/cpp-tui 3ab1f319.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Compatibility helper: the fork's fix exposes VTParser::timeout(), the
// upstream fix exposes VTParser::flush(). Both flush a pending lone ESC as a
// standalone key event. Prefer flush() when available so this test compiles
// against either version.
// ---------------------------------------------------------------------------
template <typename Parser>
auto flush_pending(Parser& parser, int) -> decltype(parser.flush())
{
  return parser.flush();
}

template <typename Parser>
auto flush_pending(Parser& parser, long) -> decltype(parser.timeout())
{
  return parser.timeout();
}

// Fallback when neither method exists (e.g. when verifying the regression by
// reverting the fix): reports no event, so the lone-ESC tests fail and expose
// the missing fix.
template <typename Parser>
cpptui::Event flush_pending(Parser&, ...)
{
  return cpptui::Event{};
}

// ---------------------------------------------------------------------------
// Unit tests: VTParser byte-level behavior (no terminal required).
// ---------------------------------------------------------------------------

TEST_CASE("VTParser: lone ESC is flushed as a standalone key event")
{
  cpptui::VTParser parser;

  // ESC alone produces no event yet — the parser waits for a follow-up byte.
  cpptui::Event e1 = parser.process(static_cast<char>(27));
  CHECK(e1.type == cpptui::EventType::None);
  CHECK(parser.in_sequence());

  // Once no more input arrives, the pending ESC is flushed as key=27.
  cpptui::Event e2 = flush_pending(parser, 0);
  CHECK(e2.type == cpptui::EventType::Key);
  CHECK(e2.key == 27);
  CHECK_FALSE(e2.alt);
  CHECK_FALSE(parser.in_sequence());
}

TEST_CASE("VTParser: incomplete CSI sequence is discarded on flush")
{
  cpptui::VTParser parser;

  parser.process(static_cast<char>(27));
  parser.process('[');
  CHECK(parser.in_sequence());

  // An incomplete CSI sequence (e.g. a truncated mouse report) must be
  // discarded, not emitted as a key event.
  cpptui::Event e = flush_pending(parser, 0);
  CHECK(e.type == cpptui::EventType::None);
  CHECK_FALSE(parser.in_sequence());
}

TEST_CASE("VTParser: ESC followed by SGR mouse report parses a mouse event")
{
  cpptui::VTParser parser;

  cpptui::Event e1 = parser.process(static_cast<char>(27));
  CHECK(e1.type == cpptui::EventType::None);
  parser.process('[');
  for (char c : std::string("<0;5;5"))
  {
    cpptui::Event e = parser.process(c);
    CHECK(e.type == cpptui::EventType::None);
  }

  // \033[<0;5;5M = left button press at column 5, row 5 (1-based).
  cpptui::Event e2 = parser.process('M');
  CHECK(e2.type == cpptui::EventType::Mouse);
  CHECK(e2.button == 0);
  CHECK(e2.x == 4);
  CHECK(e2.y == 4);
}

TEST_CASE("VTParser: ESC followed by printable key is Alt+key")
{
  cpptui::VTParser parser;

  parser.process(static_cast<char>(27));
  cpptui::Event e = parser.process('a');
  CHECK(e.type == cpptui::EventType::Key);
  CHECK(e.key == 'a');
  CHECK(e.alt);
  CHECK_FALSE(parser.in_sequence());
}

TEST_CASE("VTParser: ESC followed by ESC emits standalone Escape and retains sequence state")
{
  cpptui::VTParser parser;

  cpptui::Event e1 = parser.process(static_cast<char>(27));
  CHECK(e1.type == cpptui::EventType::None);

  // A second ESC: the first was a standalone Escape key press; the second
  // starts a new escape sequence (e.g. a mouse report).
  cpptui::Event e2 = parser.process(static_cast<char>(27));
  CHECK(e2.type == cpptui::EventType::Key);
  CHECK(e2.key == 27);
  CHECK_FALSE(e2.alt);

  // The second ESC must leave the parser ready to parse the next sequence.
  CHECK(parser.in_sequence());
  parser.process('[');
  for (char c : std::string("<0;5;5"))
  {
    cpptui::Event e = parser.process(c);
    CHECK(e.type == cpptui::EventType::None);
  }
  cpptui::Event e3 = parser.process('M');
  CHECK(e3.type == cpptui::EventType::Mouse);
  CHECK(e3.button == 0);
}

TEST_CASE("VTParser: ESC followed by ESC then flush emits second standalone Escape")
{
  cpptui::VTParser parser;

  parser.process(static_cast<char>(27));
  cpptui::Event e2 = parser.process(static_cast<char>(27));
  CHECK(e2.type == cpptui::EventType::Key);
  CHECK(e2.key == 27);
  CHECK_FALSE(e2.alt);

  // The second ESC is still pending as a new escape sequence; flushing now
  // must emit it as another standalone Escape.
  cpptui::Event e3 = flush_pending(parser, 0);
  CHECK(e3.type == cpptui::EventType::Key);
  CHECK(e3.key == 27);
  CHECK_FALSE(e3.alt);
  CHECK_FALSE(parser.in_sequence());
}

// ---------------------------------------------------------------------------
// End-to-end tests: full App event loop under a PTY.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// A lone ESC byte must reach the app as a key=27 event once the input grace
// period expires. Validates the readRawEvent timeout path end-to-end.
// ---------------------------------------------------------------------------
TEST_CASE("cpptui PTY: lone ESC is delivered as a key event")
{
  pty_test::pty_terminal pty;
  std::atomic<bool> app_exited{false};
  pty_test::pty_drainer drainer(pty.fd());

  std::thread app_thread([&]() {
    using namespace cpptui;

    try
    {
      App app;
      auto root = std::make_shared<Vertical>();
      auto label = std::make_shared<Label>(StyledText("AAAA"));
      root->add(label);

      app.register_key(27, [&]() { label->set_text(StyledText("BBBB")); });
      app.register_key('q', [&]() { app.quit(); });

      app.run(root);
    }
    catch (...)
    {
    }
    app_exited.store(true, std::memory_order_release);
  });

  pty_test::app_cleanup guard{pty, app_exited, app_thread};

  REQUIRE(drainer.wait_for("AAAA", 5000));

  // Send a lone ESC byte with no follow-up input.
  pty.send_keys("\x1b");
  REQUIRE(drainer.wait_for("BBBB", 5000));

  pty.send_keys("q");
  REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));

  app_thread.join();
  guard.joined = true;
  pty.restore();
}

// ---------------------------------------------------------------------------
// ESC immediately followed by an SGR mouse report (single burst, as when a
// mouse event races with an ESC keypress). The ESC must be delivered as a
// standalone key event and the mouse report must still parse as a mouse
// event — not be misparsed as Alt+Escape.
// ---------------------------------------------------------------------------
TEST_CASE("cpptui PTY: ESC followed by mouse report is not parsed as Alt+Escape")
{
  pty_test::pty_terminal pty;
  std::atomic<bool> app_exited{false};
  pty_test::pty_drainer drainer(pty.fd());

  std::thread app_thread([&]() {
    using namespace cpptui;

    try
    {
      App app;
      auto root = std::make_shared<Vertical>();
      auto label = std::make_shared<Label>(StyledText("AAAA"));
      root->add(label);

      app.register_key(27, [&]() { label->set_text(StyledText("BBBB")); });
      app.register_key('q', [&]() { app.quit(); });

      app.run(root);
    }
    catch (...)
    {
    }
    app_exited.store(true, std::memory_order_release);
  });

  pty_test::app_cleanup guard{pty, app_exited, app_thread};

  REQUIRE(drainer.wait_for("AAAA", 5000));

  // ESC + SGR mouse report in one burst.
  pty.send_keys("\x1b\x1b[<0;5;5M");
  REQUIRE(drainer.wait_for("BBBB", 5000));

  pty.send_keys("q");
  REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));

  app_thread.join();
  guard.joined = true;
  pty.restore();
}
