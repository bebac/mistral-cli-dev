#include <doctest/doctest.h>
#include "pty_test_harness.h"
#include "cpptui.hpp"

#include <atomic>
#include <iostream>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// Test 1: Verify Terminal construction and render work under a PTY.
// Creates a Terminal, renders an empty buffer, and verifies that the
// escape sequences for entering raw mode appear on the PTY master.
// ---------------------------------------------------------------------------
TEST_CASE("cpptui PTY: Terminal construct and render")
{
  pty_test::pty_terminal pty;
  std::atomic<bool> app_exited{false};

  std::thread app_thread([&]() {
    try {
      using namespace cpptui;
      Terminal term;
      Buffer buf(80, 24);
      Buffer prev(80, 24);
      term.render(buf, prev);
    } catch (...) {
      // Swallow exceptions — the test verifies output via the PTY.
    }
    app_exited.store(true, std::memory_order_release);
  });

  pty_test::app_cleanup guard{pty, app_exited, app_thread};

  // The Terminal constructor writes escape sequences to enter raw mode.
  // Verify the alternate screen buffer escape sequence appears.
  REQUIRE(pty.wait_for_output("\033[?1049h", 5000));

  REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));
  app_thread.join();
  guard.joined = true;
  pty.restore();
}

// ---------------------------------------------------------------------------
// Test 2: Verify Ctrl+C (raw byte 0x03) triggers cpp-tui's mandatory exit.
// cpp-tui handles Ctrl+C as a global exit in the event loop (unless a widget
// with an active selection consumes it for copy).
// ---------------------------------------------------------------------------
TEST_CASE("cpptui PTY: Ctrl+C exits the app")
{
  pty_test::pty_terminal pty;
  std::atomic<bool> app_exited{false};

  std::thread app_thread([&]() {
    using namespace cpptui;

    App app;
    auto root = std::make_shared<Vertical>();
    root->add(std::make_shared<Label>(StyledText("Ctrl+C test")));

    app.run(root);
    app_exited.store(true, std::memory_order_release);
  });

  pty_test::app_cleanup guard{pty, app_exited, app_thread};

  // Wait for the app to render.
  REQUIRE(pty.wait_for_output("Ctrl+C test", 5000));

  // Send Ctrl+C as raw byte 0x03 (ISIG is disabled in raw mode, so the
  // byte is delivered to the app as a key event with key=3, ctrl=true).
  pty.send_keys("\x03");

  // Drain during shutdown.
  REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));

  app_thread.join();
  guard.joined = true;
  pty.restore();
}

// ---------------------------------------------------------------------------
// Test 3: Verify the app responds to keypresses by updating rendered output.
// A label changes text on each keypress; we verify the new text appears in
// the PTY output after sending a key.
//
// Uses short, all-caps labels ("AAAA", "BBBB", "CCCC") so that the entire
// changed text appears as a contiguous substring in the incremental render
// diff (Terminal::render only writes changed cells, not the full screen).
//
// Uses a background drainer thread that continuously reads from the PTY
// master and accumulates output. This ensures the PTY slave's output buffer
// never fills up (which would block the app's writes), and provides a
// single source of truth for checking rendered output.
// ---------------------------------------------------------------------------
TEST_CASE("cpptui PTY: keypress updates rendered output")
{
  pty_test::pty_terminal pty;
  std::atomic<bool> app_exited{false};
  pty_test::pty_drainer drainer(pty.fd());

  std::thread app_thread([&]() {
    using namespace cpptui;

    int counter = 0;

    try {
      App app;
      auto root = std::make_shared<Vertical>();

      auto label = std::make_shared<Label>(StyledText("AAAA"));
      root->add(label);

      // Space cycles through distinctive labels.
      app.register_key(' ', [&]() {
        counter++;
        std::string text = (counter == 1) ? "BBBB" :
                           (counter == 2) ? "CCCC" : "DDDD";
        label->set_text(StyledText(text));
      });

      app.register_key('q', [&]() { app.quit(); });

      app.run(root);
    } catch (const std::exception& e) {
      std::cerr << "[test3] exception: " << e.what() << "\n";
    } catch (...) {
      std::cerr << "[test3] unknown exception\n";
    }
    app_exited.store(true, std::memory_order_release);
  });

  pty_test::app_cleanup guard{pty, app_exited, app_thread};

  // Wait for initial render.
  REQUIRE(drainer.wait_for("AAAA", 5000));

  // Press space to change the label.
  pty.send_keys(" ");
  REQUIRE(drainer.wait_for("BBBB", 5000));

  // Press space again.
  pty.send_keys(" ");
  REQUIRE(drainer.wait_for("CCCC", 5000));

  // Quit.
  pty.send_keys("q");
  REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));

  app_thread.join();
  guard.joined = true;
  pty.restore();
}