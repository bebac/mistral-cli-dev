#include <doctest/doctest.h>
#include "pty_test_harness.h"
#include "tui/tui_chat_input.h"

#include <atomic>
#include <iostream>
#include <string>
#include <thread>

using mistral::tui::ChatInput;

// ---------------------------------------------------------------------------
// Test 1: Normal text input renders, and Alt+Enter triggers on_submit.
//
// Verifies that:
//   - Typed text appears in the rendered output (TextArea rendering works)
//   - Alt+Enter (\x1b\r) fires on_submit (not Enter/insert-newline)
// ---------------------------------------------------------------------------
TEST_CASE("ChatInput PTY: text input and Alt+Enter submit")
{
  pty_test::pty_terminal pty;
  std::atomic<bool> app_exited{false};
  pty_test::pty_drainer drainer(pty.fd());

  std::thread app_thread([&]() {
    using namespace cpptui;

    try {
      App app;
      auto root = std::make_shared<Vertical>();
      root->width = 80;
      root->height = 24;

      auto status = std::make_shared<Label>(StyledText("IDLE"));
      status->fixed_height = 1;
      status->width = 80;
      status->height = 1;

      auto input = std::make_shared<ChatInput>();
      input->show_line_numbers = false;
      input->word_wrap = true;
      input->show_scrollbar = false;
      input->accepts_tab = false;
      input->fixed_height = 5;
      input->width = 78;
      input->height = 5;

      input->on_submit = [&]() {
        status->set_text(StyledText("SUBMITTED"));
      };

      root->add(status);
      root->add(input);

      app.register_key('q', [&]() { app.quit(); });

      app.run(root);
    } catch (const std::exception& e) {
      std::cerr << "[test1] exception: " << e.what() << "\n";
    } catch (...) {
      std::cerr << "[test1] unknown exception\n";
    }
    app_exited.store(true, std::memory_order_release);
  });

  pty_test::app_cleanup guard{pty, app_exited, app_thread};

  REQUIRE(drainer.wait_for("IDLE", 5000));

  // Type text — should appear in rendered output.
  pty.send_keys("hello");
  REQUIRE(drainer.wait_for("hello", 5000));

  // Alt+Enter (\x1b\r) triggers on_submit.
  pty.send_keys("\x1b\r");
  REQUIRE(drainer.wait_for("SUBMITTED", 5000));

  // Quit.
  pty.send_keys("q");
  REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));

  app_thread.join();
  guard.joined = true;
  pty.restore();
}

// ---------------------------------------------------------------------------
// Test 2: Double-Enter (Enter on empty last line with content) submits.
//
// Verifies that:
//   - First Enter inserts a newline (TextArea default behavior)
//   - Second Enter on the empty last line with content above triggers submit
// ---------------------------------------------------------------------------
TEST_CASE("ChatInput PTY: double-Enter submit")
{
  pty_test::pty_terminal pty;
  std::atomic<bool> app_exited{false};
  pty_test::pty_drainer drainer(pty.fd());

  std::thread app_thread([&]() {
    using namespace cpptui;

    try {
      App app;
      auto root = std::make_shared<Vertical>();
      root->width = 80;
      root->height = 24;

      auto status = std::make_shared<Label>(StyledText("IDLE"));
      status->fixed_height = 1;
      status->width = 80;
      status->height = 1;

      auto input = std::make_shared<ChatInput>();
      input->show_line_numbers = false;
      input->word_wrap = true;
      input->show_scrollbar = false;
      input->accepts_tab = false;
      input->fixed_height = 5;
      input->width = 78;
      input->height = 5;

      input->on_submit = [&]() {
        status->set_text(StyledText("SUBMITTED"));
      };

      root->add(status);
      root->add(input);

      app.register_key('q', [&]() { app.quit(); });

      app.run(root);
    } catch (const std::exception& e) {
      std::cerr << "[test2] exception: " << e.what() << "\n";
    } catch (...) {
      std::cerr << "[test2] unknown exception\n";
    }
    app_exited.store(true, std::memory_order_release);
  });

  pty_test::app_cleanup guard{pty, app_exited, app_thread};

  REQUIRE(drainer.wait_for("IDLE", 5000));

  // Type text.
  pty.send_keys("hi");
  REQUIRE(drainer.wait_for("hi", 5000));

  // Press Enter — inserts a newline (cursor moves to empty line 2).
  pty.send_keys("\r");

  // Press Enter again — empty last line with content above → submit.
  pty.send_keys("\r");
  REQUIRE(drainer.wait_for("SUBMITTED", 5000));

  // Quit.
  pty.send_keys("q");
  REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));

  app_thread.join();
  guard.joined = true;
  pty.restore();
}

// ---------------------------------------------------------------------------
// Test 4: A stray space after the cursor on the last line must not block
// double-Enter submit.
//
// Regression test: with a space after the cursor on the last line, every
// Enter split the line at the cursor and the space stayed on the new last
// line. The last line was never empty, so the submit condition never
// fired and double-Enter could not submit at all.
// ---------------------------------------------------------------------------
TEST_CASE("ChatInput PTY: double-Enter submits with stray space after cursor")
{
  pty_test::pty_terminal pty;
  std::atomic<bool> app_exited{false};
  pty_test::pty_drainer drainer(pty.fd());

  std::thread app_thread([&]() {
    using namespace cpptui;

    try {
      App app;
      auto root = std::make_shared<Vertical>();
      root->width = 80;
      root->height = 24;

      auto status = std::make_shared<Label>(StyledText("IDLE"));
      status->fixed_height = 1;
      status->width = 80;
      status->height = 1;

      auto input = std::make_shared<ChatInput>();
      input->show_line_numbers = false;
      input->word_wrap = true;
      input->show_scrollbar = false;
      input->accepts_tab = false;
      input->fixed_height = 5;
      input->width = 78;
      input->height = 5;

      input->on_submit = [&]() {
        status->set_text(StyledText("SUBMITTED"));
      };

      root->add(status);
      root->add(input);

      app.register_key('q', [&]() { app.quit(); });

      app.run(root);
    } catch (const std::exception& e) {
      std::cerr << "[test4] exception: " << e.what() << "\n";
    } catch (...) {
      std::cerr << "[test4] unknown exception\n";
    }
    app_exited.store(true, std::memory_order_release);
  });

  pty_test::app_cleanup guard{pty, app_exited, app_thread};

  REQUIRE(drainer.wait_for("IDLE", 5000));

  // Type text.
  pty.send_keys("hi");
  REQUIRE(drainer.wait_for("hi", 5000));

  // Enter: newline, cursor on empty line 2.
  pty.send_keys("\r");

  // Stray space on line 2, then Left so the cursor sits before it.
  pty.send_keys(" ");
  pty.send_keys("\x1b[D");

  // Enter on the whitespace-only last line must submit.
  pty.send_keys("\r");
  REQUIRE(drainer.wait_for("SUBMITTED", 5000));

  // Quit.
  pty.send_keys("q");
  REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));

  app_thread.join();
  guard.joined = true;
  pty.restore();
}

// ---------------------------------------------------------------------------
// Test 5: Submitted text is trimmed of trailing whitespace.
//
// Verifies that:
//   - A trailing space on the last content line is not sent
//   - The blank confirmation line of the double-Enter is not sent
// ---------------------------------------------------------------------------
TEST_CASE("ChatInput PTY: submitted text has trailing whitespace trimmed")
{
  pty_test::pty_terminal pty;
  std::atomic<bool> app_exited{false};
  pty_test::pty_drainer drainer(pty.fd());

  std::thread app_thread([&]() {
    using namespace cpptui;

    try {
      App app;
      auto root = std::make_shared<Vertical>();
      root->width = 80;
      root->height = 24;

      auto status = std::make_shared<Label>(StyledText("IDLE"));
      status->fixed_height = 1;
      status->width = 80;
      status->height = 1;

      auto input = std::make_shared<ChatInput>();
      input->show_line_numbers = false;
      input->word_wrap = true;
      input->show_scrollbar = false;
      input->accepts_tab = false;
      input->fixed_height = 5;
      input->width = 78;
      input->height = 5;

      input->on_submit = [&]() {
        status->set_text(StyledText(
          "LEN=" + std::to_string(input->get_submit_text().size())));
      };

      root->add(status);
      root->add(input);

      app.register_key('q', [&]() { app.quit(); });

      app.run(root);
    } catch (const std::exception& e) {
      std::cerr << "[test5] exception: " << e.what() << "\n";
    } catch (...) {
      std::cerr << "[test5] unknown exception\n";
    }
    app_exited.store(true, std::memory_order_release);
  });

  pty_test::app_cleanup guard{pty, app_exited, app_thread};

  REQUIRE(drainer.wait_for("IDLE", 5000));

  // Type text with a trailing space.
  pty.send_keys("hi ");
  REQUIRE(drainer.wait_for("hi", 5000));

  // Enter: newline (last line "hi " is not blank).
  pty.send_keys("\r");

  // Enter: empty last line → submit. The submitted text must be "hi"
  // (trailing space and blank confirmation line trimmed), not "hi \n".
  pty.send_keys("\r");
  REQUIRE(drainer.wait_for("LEN=2", 5000));

  // Quit.
  pty.send_keys("q");
  REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));

  app_thread.join();
  guard.joined = true;
  pty.restore();
}

// ---------------------------------------------------------------------------
// Test 3: Single-key approval mode intercepts y/n/r/a/? keys.
//
// Verifies that:
//   - In single_key mode, pressing 'y' calls on_approval_key('y')
//   - The key is intercepted (not inserted into the text area)
//   - 'q' still quits (registered key events bypass the focused widget)
// ---------------------------------------------------------------------------
TEST_CASE("ChatInput PTY: single-key approval mode")
{
  pty_test::pty_terminal pty;
  std::atomic<bool> app_exited{false};
  pty_test::pty_drainer drainer(pty.fd());

  std::thread app_thread([&]() {
    using namespace cpptui;

    try {
      App app;
      auto root = std::make_shared<Vertical>();
      root->width = 80;
      root->height = 24;

      auto status = std::make_shared<Label>(StyledText("WAITING"));
      status->fixed_height = 1;
      status->width = 80;
      status->height = 1;

      auto input = std::make_shared<ChatInput>();
      input->show_line_numbers = false;
      input->word_wrap = true;
      input->show_scrollbar = false;
      input->accepts_tab = false;
      input->fixed_height = 5;
      input->width = 78;
      input->height = 5;

      input->on_approval_key = [&](char ch) {
        status->set_text(StyledText(std::string("APPROVED:") + ch));
      };

      // Enter single-key approval mode before running.
      input->set_approval_mode(ChatInput::approval_mode::single_key);

      root->add(status);
      root->add(input);

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

  REQUIRE(drainer.wait_for("WAITING", 5000));

  // Press 'y' — should trigger on_approval_key, not insert 'y' into text.
  pty.send_keys("y");
  REQUIRE(drainer.wait_for("APPROVED:y", 5000));

  // Quit. 'q' is a registered key event, processed before the focused
  // widget, so it bypasses the single_key interception.
  pty.send_keys("q");
  REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));

  app_thread.join();
  guard.joined = true;
  pty.restore();
}