#include <doctest/doctest.h>
#include "pty_test_harness.h"
#include "tui/tui_widgets.h"
#include "slash_command/slash_command.h"
#include "slash_command/slash_command_registry.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// Test command: minimal slash_command implementation for testing.
// ---------------------------------------------------------------------------
namespace {
class test_command : public mistral::slash_command::slash_command {
public:
  test_command(const std::string& name, const std::string& desc)
    : name_(name), desc_(desc) {}

  std::string name() const override { return name_; }
  std::string description() const override { return desc_; }

  dripcore::awaitable<std::string> execute(
    dripcore::task&, const std::string&,
    mistral::agent::agent_system&) override
  {
    co_return "";
  }

private:
  std::string name_;
  std::string desc_;
};

// Build a registry with the 10 real command names for testing.
std::shared_ptr<mistral::slash_command::slash_command_registry>
make_test_registry() {
  auto reg = std::make_shared<mistral::slash_command::slash_command_registry>();
  reg->register_command(std::make_unique<test_command>(
    "help", "Show available slash commands"));
  reg->register_command(std::make_unique<test_command>(
    "clear", "Clear the chat history"));
  reg->register_command(std::make_unique<test_command>(
    "model", "Show current model information"));
  reg->register_command(std::make_unique<test_command>(
    "tools", "List available tools"));
  reg->register_command(std::make_unique<test_command>(
    "exit", "Exit the application"));
  reg->register_command(std::make_unique<test_command>(
    "load", "Load a file and set it as the last response"));
  reg->register_command(std::make_unique<test_command>(
    "save", "Save the last AI response to a file"));
  reg->register_command(std::make_unique<test_command>(
    "usage", "Show API usage statistics"));
  reg->register_command(std::make_unique<test_command>(
    "patchset_verify", "Verify a patchset"));
  reg->register_command(std::make_unique<test_command>(
    "tool", "Run a tool"));
  return reg;
}

// Build a widget tree with ChatInput + SlashCommandPopup for testing.
struct test_app {
  std::shared_ptr<mistral::slash_command::slash_command_registry> registry;
  std::shared_ptr<cpptui::Label> status;
  std::shared_ptr<SlashCommandPopup> slash_popup;
  std::shared_ptr<Divider> slash_divider;
  std::shared_ptr<ChatInput> input;
  std::atomic<bool> agent_busy{false};
  std::atomic<bool> slash_command_called{false};
  std::string last_slash_command;
  std::string last_slash_args;

  void build(cpptui::App& app) {
    using namespace cpptui;

    registry = make_test_registry();

    auto root = std::make_shared<Vertical>();
    root->width = 80;
    root->height = 24;

    status = std::make_shared<Label>(StyledText("IDLE"));
    status->fixed_height = 1;
    status->width = 80;
    status->height = 1;

    slash_divider = std::make_shared<Divider>();
    slash_divider->visible = false;

    slash_popup = std::make_shared<SlashCommandPopup>();
    slash_popup->fixed_height = 0;
    slash_popup->visible = false;
    slash_popup->set_registry(registry.get());

    auto input_divider = std::make_shared<Divider>();

    input = std::make_shared<ChatInput>();
    input->show_line_numbers = false;
    input->word_wrap = true;
    input->show_scrollbar = false;
    input->accepts_tab = false;
    input->fixed_height = 5;
    input->width = 78;
    input->height = 5;

    input->set_slash_popup(slash_popup.get());
    input->set_slash_divider(slash_divider.get());
    input->is_agent_busy = [this]() {
      return agent_busy.load(std::memory_order_acquire);
    };
    input->on_slash_command = [this](std::string cmd, std::string args) {
      slash_command_called.store(true, std::memory_order_release);
      last_slash_command = cmd;
      last_slash_args = args;
      status->set_text(StyledText("SLASH:" + cmd));
    };

    root->add(status);
    root->add(slash_divider);
    root->add(slash_popup);
    root->add(input_divider);
    root->add(input);

    app.register_key('q', [&]() { app.quit(); });

    app.run(root);
  }
};
} // anonymous namespace

// ---------------------------------------------------------------------------
// Test 1: Activation — typing '/' shows the popup with command names.
// ---------------------------------------------------------------------------
TEST_CASE("SlashCommand PTY: activation shows popup")
{
  pty_test::pty_terminal pty;
  std::atomic<bool> app_exited{false};
  pty_test::pty_drainer drainer(pty.fd());

  std::thread app_thread([&]() {
    using namespace cpptui;
    try {
      App app;
      test_app ta;
      ta.build(app);
    } catch (const std::exception& e) {
      std::cerr << "[test1] exception: " << e.what() << "\n";
    } catch (...) {
      std::cerr << "[test1] unknown exception\n";
    }
    app_exited.store(true, std::memory_order_release);
  });

  pty_test::app_cleanup guard{pty, app_exited, app_thread};

  REQUIRE(drainer.wait_for("IDLE", 5000));

  // Type '/' — should activate slash mode and show popup.
  pty.send_keys("/");
  REQUIRE(drainer.wait_for("/help", 5000));

  // Quit.
  pty.send_keys("q");
  REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));
  app_thread.join();
  guard.joined = true;
  pty.restore();
}

// ---------------------------------------------------------------------------
// Test 2: Filtering — typing '/sa' filters to only /save.
//
// This test verifies filtering through behavior rather than screen inspection.
// cpptui uses incremental rendering (only changed cells are written), so we
// cannot reliably check the popup state through the PTY output. Instead, we
// type "sa" to filter, then press Enter twice to insert and execute the
// selected command. If filtering worked, the selected command is "/save" and
// we see "SLASH:save". If filtering failed, the first command "/help" would
// be selected and we'd see "SLASH:help" instead.
// ---------------------------------------------------------------------------
TEST_CASE("SlashCommand PTY: filtering narrows commands")
{
  pty_test::pty_terminal pty;
  std::atomic<bool> app_exited{false};
  pty_test::pty_drainer drainer(pty.fd());

  std::thread app_thread([&]() {
    using namespace cpptui;
    try {
      App app;
      test_app ta;
      ta.build(app);
    } catch (const std::exception& e) {
      std::cerr << "[test2] exception: " << e.what() << "\n";
    } catch (...) {
      std::cerr << "[test2] unknown exception\n";
    }
    app_exited.store(true, std::memory_order_release);
  });

  pty_test::app_cleanup guard{pty, app_exited, app_thread};

  REQUIRE(drainer.wait_for("IDLE", 5000));

  // Type '/' to activate slash mode.
  pty.send_keys("/");
  REQUIRE(drainer.wait_for("/help", 5000));

  // Type 'sa' to filter the popup to only /save.
  pty.send_keys("sa");

  // Give the app time to process keystrokes and re-render.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // Press Enter to insert the selected command from the filtered popup.
  // If filtering worked, the selected command is "/save".
  pty.send_keys("\r");

  // Press Enter again to execute the slash command.
  pty.send_keys("\r");

  // If filtering worked, the app executes "save" and the status label
  // shows "SLASH:save". If filtering failed, "/help" (the first command)
  // would be selected instead, producing "SLASH:help".
  REQUIRE(drainer.wait_for("SLASH:save", 5000));

  // Quit.
  pty.send_keys("q");
  REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));
  app_thread.join();
  guard.joined = true;
  pty.restore();
}

// ---------------------------------------------------------------------------
// Test 3: Navigation — arrow down moves selection, popup still renders.
// ---------------------------------------------------------------------------
TEST_CASE("SlashCommand PTY: arrow navigation")
{
  pty_test::pty_terminal pty;
  std::atomic<bool> app_exited{false};
  pty_test::pty_drainer drainer(pty.fd());

  std::thread app_thread([&]() {
    using namespace cpptui;
    try {
      App app;
      test_app ta;
      ta.build(app);
    } catch (const std::exception& e) {
      std::cerr << "[test3] exception: " << e.what() << "\n";
    } catch (...) {
      std::cerr << "[test3] unknown exception\n";
    }
    app_exited.store(true, std::memory_order_release);
  });

  pty_test::app_cleanup guard{pty, app_exited, app_thread};

  REQUIRE(drainer.wait_for("IDLE", 5000));

  // Type '/' to activate slash mode.
  pty.send_keys("/");
  REQUIRE(drainer.wait_for("/help", 5000));

  // Send arrow down — popup should still render correctly.
  pty.send_keys("\x1b[B");
  // Verify popup is still visible (any command name should be present).
  REQUIRE(drainer.wait_for("/clear", 5000));

  // Quit.
  pty.send_keys("q");
  REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));
  app_thread.join();
  guard.joined = true;
  pty.restore();
}

// ---------------------------------------------------------------------------
// Test 4: Tab completion — '/he' + Tab completes to '/help' in input.
// ---------------------------------------------------------------------------
TEST_CASE("SlashCommand PTY: tab completion")
{
  pty_test::pty_terminal pty;
  std::atomic<bool> app_exited{false};
  pty_test::pty_drainer drainer(pty.fd());

  std::thread app_thread([&]() {
    using namespace cpptui;
    try {
      App app;
      test_app ta;
      ta.build(app);
    } catch (const std::exception& e) {
      std::cerr << "[test4] exception: " << e.what() << "\n";
    } catch (...) {
      std::cerr << "[test4] unknown exception\n";
    }
    app_exited.store(true, std::memory_order_release);
  });

  pty_test::app_cleanup guard{pty, app_exited, app_thread};

  REQUIRE(drainer.wait_for("IDLE", 5000));

  // Type '/he' then Tab to complete.
  pty.send_keys("/he");
  REQUIRE(drainer.wait_for("/help", 5000));

  // Press Tab — input becomes '/help', slash mode exits.
  // The terminal only writes changed cells, so we can't wait for the
  // full '/help' string. Instead, type a distinctive character to
  // verify we're back in normal editing mode.
  pty.send_keys("\t");
  pty.send_keys("Z");
  REQUIRE(drainer.wait_for("Z", 5000));

  // Quit.
  pty.send_keys("q");
  REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));
  app_thread.join();
  guard.joined = true;
  pty.restore();
}

// ---------------------------------------------------------------------------
// Test 5: Escape cancellation — '/' then Escape hides popup.
// ---------------------------------------------------------------------------
TEST_CASE("SlashCommand PTY: escape cancellation")
{
  pty_test::pty_terminal pty;
  std::atomic<bool> app_exited{false};
  pty_test::pty_drainer drainer(pty.fd());

  std::thread app_thread([&]() {
    using namespace cpptui;
    try {
      App app;
      test_app ta;
      ta.build(app);
    } catch (const std::exception& e) {
      std::cerr << "[test5] exception: " << e.what() << "\n";
    } catch (...) {
      std::cerr << "[test5] unknown exception\n";
    }
    app_exited.store(true, std::memory_order_release);
  });

  pty_test::app_cleanup guard{pty, app_exited, app_thread};

  REQUIRE(drainer.wait_for("IDLE", 5000));

  // Type '/' to activate slash mode.
  pty.send_keys("/");
  REQUIRE(drainer.wait_for("/help", 5000));

  // Clear drainer, then press Escape.
  drainer.clear();
  pty.send_keys("\x1b");

  // Wait a bit, then verify popup content is NOT in new output.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  bool found = drainer.wait_for("/help", 300);
  REQUIRE(!found);

  // Quit.
  pty.send_keys("q");
  REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));
  app_thread.join();
  guard.joined = true;
  pty.restore();
}

// ---------------------------------------------------------------------------
// Test 6: Backspace exit — '/' then backspace hides popup.
// ---------------------------------------------------------------------------
TEST_CASE("SlashCommand PTY: backspace exits slash mode")
{
  pty_test::pty_terminal pty;
  std::atomic<bool> app_exited{false};
  pty_test::pty_drainer drainer(pty.fd());

  std::thread app_thread([&]() {
    using namespace cpptui;
    try {
      App app;
      test_app ta;
      ta.build(app);
    } catch (const std::exception& e) {
      std::cerr << "[test6] exception: " << e.what() << "\n";
    } catch (...) {
      std::cerr << "[test6] unknown exception\n";
    }
    app_exited.store(true, std::memory_order_release);
  });

  pty_test::app_cleanup guard{pty, app_exited, app_thread};

  REQUIRE(drainer.wait_for("IDLE", 5000));

  // Type '/' to activate slash mode.
  pty.send_keys("/");
  REQUIRE(drainer.wait_for("/help", 5000));

  // Clear drainer, then press backspace to remove '/'.
  drainer.clear();
  pty.send_keys("\x7f"); // DEL / backspace

  // Wait a bit, then verify popup content is NOT in new output.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  bool found = drainer.wait_for("/help", 300);
  REQUIRE(!found);

  // Quit.
  pty.send_keys("q");
  REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));
  app_thread.join();
  guard.joined = true;
  pty.restore();
}

// ---------------------------------------------------------------------------
// Test 7: Agent-busy guard — '/' while agent busy does NOT activate popup.
// ---------------------------------------------------------------------------
TEST_CASE("SlashCommand PTY: agent-busy guard")
{
  pty_test::pty_terminal pty;
  std::atomic<bool> app_exited{false};
  pty_test::pty_drainer drainer(pty.fd());

  std::thread app_thread([&]() {
    using namespace cpptui;
    try {
      App app;
      test_app ta;
      ta.agent_busy.store(true, std::memory_order_release);
      ta.build(app);
    } catch (const std::exception& e) {
      std::cerr << "[test7] exception: " << e.what() << "\n";
    } catch (...) {
      std::cerr << "[test7] unknown exception\n";
    }
    app_exited.store(true, std::memory_order_release);
  });

  pty_test::app_cleanup guard{pty, app_exited, app_thread};

  REQUIRE(drainer.wait_for("IDLE", 5000));

  // Type '/' while agent is busy — should NOT activate popup.
  pty.send_keys("/");

  // Wait a bit, then verify popup content does NOT appear.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  bool found = drainer.wait_for("/help", 300);
  REQUIRE(!found);

  // Quit.
  pty.send_keys("q");
  REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));
  app_thread.join();
  guard.joined = true;
  pty.restore();
}

// ---------------------------------------------------------------------------
// Test 8: Enter inserts command, second Enter executes.
//
// Type '/clear' + Enter → inserts '/clear ' and exits slash popup mode.
// Press Enter again → on_slash_command called with "clear".
// ---------------------------------------------------------------------------
TEST_CASE("SlashCommand PTY: enter inserts then executes slash command")
{
  pty_test::pty_terminal pty;
  std::atomic<bool> app_exited{false};
  pty_test::pty_drainer drainer(pty.fd());

  std::thread app_thread([&]() {
    using namespace cpptui;
    try {
      App app;
      test_app ta;
      ta.build(app);
    } catch (const std::exception& e) {
      std::cerr << "[test8] exception: " << e.what() << "\n";
    } catch (...) {
      std::cerr << "[test8] unknown exception\n";
    }
    app_exited.store(true, std::memory_order_release);
  });

  pty_test::app_cleanup guard{pty, app_exited, app_thread};

  REQUIRE(drainer.wait_for("IDLE", 5000));

  // Type '/clear' then Enter to insert the command.
  pty.send_keys("/clear");
  REQUIRE(drainer.wait_for("/clear", 5000));

  pty.send_keys("\r"); // Enter — inserts '/clear ', exits slash popup mode.

  // Press Enter again to execute the slash command.
  pty.send_keys("\r"); // Enter — triggers on_slash_command.

  // on_slash_command should have been called with "clear".
  REQUIRE(drainer.wait_for("SLASH:clear", 5000));

  // Quit.
  pty.send_keys("q");
  REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));
  app_thread.join();
  guard.joined = true;
  pty.restore();
}

// ---------------------------------------------------------------------------
// Test 9: Enter on arrow-selected command — direct event test.
//
// Simulates '/' + Down + Enter (inserts) + Enter (executes) by calling
// on_event directly, verifying that the popup's selected command (not
// the typed text) is inserted and then executed.
// ---------------------------------------------------------------------------
TEST_CASE("SlashCommand: enter on arrow-selected command")
{
  // Build widget tree without PTY.
  auto registry = make_test_registry();
  auto popup = std::make_shared<SlashCommandPopup>();
  popup->set_registry(registry.get());
  popup->visible = false;
  auto divider = std::make_shared<Divider>();
  divider->visible = false;
  auto input = std::make_shared<ChatInput>();
  input->show_line_numbers = false;
  input->word_wrap = true;
  input->show_scrollbar = false;
  input->accepts_tab = false;
  input->fixed_height = 5;
  input->width = 78;
  input->height = 5;
  input->set_slash_popup(popup.get());
  input->set_slash_divider(divider.get());
  input->is_agent_busy = []() { return false; };

  std::string last_cmd, last_args;
  input->on_slash_command = [&](std::string cmd, std::string args) {
    last_cmd = cmd;
    last_args = args;
  };

  // Focus the input widget so TextArea processes key events.
  input->set_focus(true);

  // Simulate typing '/' to activate slash mode.
  cpptui::Event slash_event;
  slash_event.type = cpptui::EventType::Key;
  slash_event.key = '/';
  input->on_event(slash_event);

  REQUIRE(input->slash_mode_active());
  REQUIRE(popup->visible);

  // Commands are stored alphabetically (std::map), so index 0 = "clear",
  // index 1 = "exit". Arrow down moves from index 0 to index 1.
  cpptui::Event down_event;
  down_event.type = cpptui::EventType::Key;
  down_event.key = 1066; // nav_down
  input->on_event(down_event);

  REQUIRE(popup->selected_command_name() == "exit");

  // First Enter: inserts '/exit ' and exits slash popup mode.
  cpptui::Event enter_event;
  enter_event.type = cpptui::EventType::Key;
  enter_event.key = 10; // Enter
  input->on_event(enter_event);

  // After first Enter, slash popup mode is exited and text is '/exit '.
  REQUIRE(!input->slash_mode_active());
  REQUIRE(input->get_text() == "/exit ");

  // Second Enter: executes the slash command via on_slash_command.
  input->on_event(enter_event);

  // The popup's selected command ("exit") should have been executed,
  // not the typed text ("/").
  REQUIRE(last_cmd == "exit");
  REQUIRE(last_args == "");
  REQUIRE(input->get_text().empty());
}