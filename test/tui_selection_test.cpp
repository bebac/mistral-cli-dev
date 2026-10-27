#include "cpptui.hpp"
#include "pty_test_harness.h"
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// Test 1: Mouse selection and Ctrl+C copy on a Label with UTF-8 text.
//
// Verifies that:
//   - SGR mouse events select text in a Label
//   - Ctrl+C copies the selected text via OSC 52
//   - The OSC 52 base64 payload matches the selected text (UTF-8 safe)
//
// "café" in UTF-8 is 5 bytes (é = 0xC3 0xA9).  Before the fix, get_selected_text
// used raw substr() with character indices as byte offsets, garbling multi-byte
// text.  After the fix, it uses TextHelper::utf8_substr().
// ---------------------------------------------------------------------------
TEST_CASE("Selection PTY: mouse select + Ctrl+C copy (UTF-8)")
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

      auto label = std::make_shared<Label>(StyledText("café test"));
      root->add(label);

      app.register_key('q', [&]() { app.quit(); });
      app.run(root);
    }
    catch (const std::exception& e)
    {
      std::cerr << "[sel1] exception: " << e.what() << "\n";
    }
    catch (...)
    {
      std::cerr << "[sel1] unknown exception\n";
    }
    app_exited.store(true, std::memory_order_release);
  });

  pty_test::app_cleanup guard{pty, app_exited, app_thread};

  REQUIRE(drainer.wait_for("café test", 5000));

  // Mouse select "café" (chars 0–3):
  // SGR mouse: \033[<b;x;yM (press/drag), \033[<b;x;ym (release)
  // x,y are 1-based; cpptui parseSGR subtracts 1.
  // Label is at (0,0).  Press at char 0 (x=1), drag to char 3 (x=4).
  // With inclusive_drag, selection range becomes [0, 4) = "café".
  pty.send_keys("\033[<0;1;1M");  // Press left button at (0,0)
  pty.send_keys("\033[<32;4;1M"); // Drag to (3,0)
  pty.send_keys("\033[<0;4;1m");  // Release at (3,0)

  // Give the app time to process mouse events.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Ctrl+C to copy selection.
  pty.send_keys("\x03");

  // The OSC 52 sequence: \033]52;c;<base64>\x07
  // "café" in UTF-8 base64 = "Y2Fmw6k="
  REQUIRE(drainer.wait_for("\033]52;c;Y2Fmw6k=", 5000));

  // Quit.
  pty.send_keys("q");
  REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));
  app_thread.join();
  guard.joined = true;
  pty.restore();
}

// ---------------------------------------------------------------------------
// Test 2: Ctrl+A select all + Ctrl+C copy on a Label with UTF-8 text.
//
// Verifies that:
//   - Ctrl+A selects all text (UTF-8 safe count_codepoints, not byte length)
//   - Ctrl+C copies the full text via OSC 52
//   - The OSC 52 base64 payload matches the full text
//
// Before the fix, select_all used text_.length() (byte count = 10 for
// "café test"), which over-selected past the last character.  After the fix,
// it uses count_codepoints() (char count = 9).
// ---------------------------------------------------------------------------
TEST_CASE("Selection PTY: Ctrl+A select all + Ctrl+C copy (UTF-8)")
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

      auto label = std::make_shared<Label>(StyledText("café test"));
      root->add(label);

      app.register_key('q', [&]() { app.quit(); });
      app.run(root);
    }
    catch (const std::exception& e)
    {
      std::cerr << "[sel2] exception: " << e.what() << "\n";
    }
    catch (...)
    {
      std::cerr << "[sel2] unknown exception\n";
    }
    app_exited.store(true, std::memory_order_release);
  });

  pty_test::app_cleanup guard{pty, app_exited, app_thread};

  REQUIRE(drainer.wait_for("café test", 5000));

  // Click to focus the Label.
  pty.send_keys("\033[<0;1;1M"); // Press at (0,0)
  pty.send_keys("\033[<0;1;1m"); // Release at (0,0)

  // Give the app time to process the click.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Ctrl+A to select all.
  pty.send_keys("\x01");

  // Give the app time to process Ctrl+A.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Ctrl+C to copy.
  pty.send_keys("\x03");

  // "café test" in UTF-8 base64 = "Y2Fmw6kgdGVzdA=="
  REQUIRE(drainer.wait_for("\033]52;c;Y2Fmw6kgdGVzdA==", 5000));

  // Quit.
  pty.send_keys("q");
  REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));
  app_thread.join();
  guard.joined = true;
  pty.restore();
}
