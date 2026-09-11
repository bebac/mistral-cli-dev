#include <doctest/doctest.h>
#include "pty_test_harness.h"
#include "tui/tui_widgets.h"
#include "tui/tui_slash_command_popup.h"
#include "tui/tui_chat_input.h"
#include "slash_command/slash_command.h"
#include "slash_command/slash_command_registry.h"
#include "slash_command/slash_command_executor_task.h"

#include <dripcore/scheduler.h>
#include <dripcore/channel.h>
#include <dripcore/task.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

using mistral::tui::ChatInput;
using mistral::tui::Divider;
using mistral::tui::SlashCommandPopup;

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

// Test command that returns a known response string.
class response_command : public mistral::slash_command::slash_command {
public:
  response_command(const std::string& name, const std::string& desc,
                    const std::string& response)
    : name_(name), desc_(desc), response_(response) {}

  std::string name() const override { return name_; }
  std::string description() const override { return desc_; }

  dripcore::awaitable<std::string> execute(
    dripcore::task&, const std::string&,
    mistral::agent::agent_system&) override
  {
    co_return response_;
  }

private:
  std::string name_;
  std::string desc_;
  std::string response_;
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

// Build a registry with commands that return known responses for
// execution tests.
std::shared_ptr<mistral::slash_command::slash_command_registry>
make_exec_test_registry() {
  auto reg = std::make_shared<mistral::slash_command::slash_command_registry>();
  reg->register_command(std::make_unique<response_command>(
    "help", "Show available slash commands",
    "Available commands: /help /clear /model /tools /exit /save /load /usage"));
  reg->register_command(std::make_unique<response_command>(
    "save", "Save the last AI response to a file",
    "Saved response to file"));
  reg->register_command(std::make_unique<response_command>(
    "clear", "Clear the chat history",
    "Chat history cleared"));
  return reg;
}

// Thread-safe slash command queue (mirrors tui_main.cpp).
struct slash_command_queue
{
  std::mutex mtx;
  std::deque<mistral::slash_command::slash_command_request> requests;

  void push(mistral::slash_command::slash_command_request req)
  {
    std::lock_guard<std::mutex> lock(mtx);
    requests.push_back(std::move(req));
  }

  std::deque<mistral::slash_command::slash_command_request> drain()
  {
    std::lock_guard<std::mutex> lock(mtx);
    std::deque<mistral::slash_command::slash_command_request> result;
    result.swap(requests);
    return result;
  }
};

// Response receiver task: receives from slash_cmd_response_ch and
// posts responses to the TUI via App::post. Spawned as a separate
// top-level task in the scheduler.
class slash_response_receiver : public dripcore::task
{
public:
  using post_fn = std::function<void(std::function<void()>)>;
  using append_fn = std::function<void(const std::string&)>;

  slash_response_receiver(
    std::shared_ptr<dripcore::channel<std::string>> response_ch,
    post_fn post,
    append_fn append_response
  )
    : response_ch_(std::move(response_ch))
    , post_(std::move(post))
    , append_response_(std::move(append_response))
  {
  }

  dripcore::awaitable<> body() override
  {
    while (true)
    {
      auto response = co_await response_ch_->recv(this);
      if (!response) break;
      post_([fn = append_response_, text = std::move(*response)]() {
        fn(text);
      });
    }
    co_return;
  }

private:
  std::shared_ptr<dripcore::channel<std::string>> response_ch_;
  post_fn post_;
  append_fn append_response_;
};

// Slash command bridge task: drains the TUI queue and sends to executor.
class slash_bridge_task : public dripcore::task
{
public:
  slash_bridge_task(
    int pipe_read_fd,
    std::shared_ptr<dripcore::channel<mistral::slash_command::slash_command_request>> cmd_request_ch,
    std::shared_ptr<dripcore::channel<std::string>> response_ch,
    slash_command_queue& queue
  )
    : pipe_read_fd_(pipe_read_fd)
    , cmd_request_ch_(std::move(cmd_request_ch))
    , response_ch_(std::move(response_ch))
    , queue_(queue)
  {
  }

  dripcore::awaitable<> body() override
  {
    while (true)
    {
      co_await readable(pipe_read_fd_);

      char buf[16];
      ssize_t n = ::read(pipe_read_fd_, buf, sizeof(buf));

      if (n <= 0) {
        cmd_request_ch_->close();
        response_ch_->close();
        co_return;
      }

      // Drain all pending requests and send to executor.
      auto requests = queue_.drain();
      for (auto& req : requests) {
        cmd_request_ch_->try_send(std::move(req));
      }
    }
    co_return;
  }

private:
  int pipe_read_fd_;
  std::shared_ptr<dripcore::channel<mistral::slash_command::slash_command_request>> cmd_request_ch_;
  std::shared_ptr<dripcore::channel<std::string>> response_ch_;
  slash_command_queue& queue_;
};

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

// ---------------------------------------------------------------------------
// Execution test infrastructure: test_exec_app includes the full slash
// command execution pipeline (dripcore scheduler + executor + bridge).
// ---------------------------------------------------------------------------

// Test app with full execution pipeline.
struct test_exec_app {
  std::shared_ptr<mistral::slash_command::slash_command_registry> registry;
  std::shared_ptr<cpptui::Label> status;
  std::shared_ptr<SlashCommandPopup> slash_popup;
  std::shared_ptr<Divider> slash_divider;
  std::shared_ptr<ChatInput> input;
  std::atomic<bool> agent_busy{false};
  std::atomic<bool> app_exited{false};
  std::shared_ptr<std::atomic<bool>> response_received;
  std::shared_ptr<std::string> last_response;

  // Slash command pipeline.
  slash_command_queue slash_cmd_queue;
  int slash_cmd_pipe_fds[2];
  std::shared_ptr<dripcore::channel<mistral::slash_command::slash_command_request>> cmd_request_ch;
  std::shared_ptr<dripcore::channel<std::string>> slash_cmd_response_ch;
  std::thread dripcore_thread;

  // Content area for responses.
  std::shared_ptr<cpptui::Paragraph> content;
  std::shared_ptr<cpptui::ScrollableVertical> scroll;
  cpptui::StyledText content_styled;

  void build(cpptui::App& app) {
    using namespace cpptui;

    registry = make_exec_test_registry();

    // Create slash command pipe.
    if (::pipe(slash_cmd_pipe_fds) != 0) {
      std::cerr << "[exec] failed to create slash command pipe\n";
      return;
    }

    // Create channels.
    cmd_request_ch = std::make_shared<dripcore::channel<mistral::slash_command::slash_command_request>>(4);
    slash_cmd_response_ch = std::make_shared<dripcore::channel<std::string>>(4);

    auto root = std::make_shared<Vertical>();
    root->width = 80;
    root->height = 24;

    status = std::make_shared<Label>(StyledText("IDLE"));
    status->fixed_height = 1;
    status->width = 80;
    status->height = 1;

    // Content area.
    content = std::make_shared<Paragraph>();
    content->word_wrap = true;
    content->preserve_whitespace = true;
    content->fixed_height = 1;
    content->width = 80;
    content->height = 1;

    content_styled.add("READY\n");
    content->set_styled(content_styled);

    scroll = std::make_shared<ScrollableVertical>();
    scroll->add(content);
    scroll->focusable = false;

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

    // Response handler: updates the status label with the response text.
    // Using the status label (always visible, 1 line) instead of a content
    // area avoids scroll/layout complexity in tests.
    auto append_response =
      [this](const std::string& text) {
        *last_response = text;
        response_received->store(true, std::memory_order_release);
        // Truncate to fit the status label width.
        std::string display = text.substr(0, 78);
        status->set_text(StyledText("RESP:" + display));
      };

    input->on_slash_command = [this, &app](std::string cmd, std::string args) {
      if (cmd == "exit") {
        app.quit();
        return;
      }
      if (agent_busy.load(std::memory_order_acquire)) return;

      slash_cmd_queue.push({cmd, args});
      char wakeup = '1';
      ::write(slash_cmd_pipe_fds[1], &wakeup, 1);
    };

    root->add(status);
    root->add(scroll);
    root->add(slash_divider);
    root->add(slash_popup);
    root->add(input_divider);
    root->add(input);

    app.register_key('q', [&]() { app.quit(); });

    // Launch dripcore scheduler in background thread.
    dripcore_thread = std::thread([&]() {
      dripcore::scheduler sched;

      // Spawn executor task.
      sched.spawn<mistral::slash_command::slash_command_executor_task>(
        cmd_request_ch, slash_cmd_response_ch, registry);

      // Spawn response receiver task (separate top-level task).
      sched.spawn<slash_response_receiver>(
        slash_cmd_response_ch,
        [&app](std::function<void()> cb) { app.post(std::move(cb)); },
        append_response
      );

      // Spawn bridge task.
      sched.spawn<slash_bridge_task>(
        slash_cmd_pipe_fds[0], cmd_request_ch, slash_cmd_response_ch,
        std::ref(slash_cmd_queue)
      );

      sched.run();
    });

    app.run(root);
    app_exited.store(true, std::memory_order_release);
  }

  void shutdown() {
    if (slash_cmd_pipe_fds[1] >= 0) {
      ::close(slash_cmd_pipe_fds[1]);
      slash_cmd_pipe_fds[1] = -1;
    }
    if (dripcore_thread.joinable()) {
      dripcore_thread.join();
    }
    if (slash_cmd_pipe_fds[0] >= 0) {
      ::close(slash_cmd_pipe_fds[0]);
      slash_cmd_pipe_fds[0] = -1;
    }
  }

  ~test_exec_app() {
    shutdown();
  }
};

// ---------------------------------------------------------------------------
// Test 10: Basic execution — /help returns known response text.
// ---------------------------------------------------------------------------
TEST_CASE("SlashCommand PTY: basic execution shows response")
{
  pty_test::pty_terminal pty;
  std::atomic<bool> app_exited{false};
  pty_test::pty_drainer drainer(pty.fd());
  auto response_received = std::make_shared<std::atomic<bool>>(false);
  auto last_response = std::make_shared<std::string>();

  std::thread app_thread([&]() {
    using namespace cpptui;
    try {
      App app;
      test_exec_app ta;
      ta.response_received = response_received;
      ta.last_response = last_response;
      ta.build(app);
    } catch (const std::exception& e) {
      std::cerr << "[test10] exception: " << e.what() << "\n";
    } catch (...) {
      std::cerr << "[test10] unknown exception\n";
    }
    app_exited.store(true, std::memory_order_release);
  });

  pty_test::app_cleanup guard{pty, app_exited, app_thread};

  REQUIRE(drainer.wait_for("READY", 5000));

  // Type '/help' then Enter to insert from popup, then Enter to execute.
  pty.send_keys("/help");
  pty.send_keys("\r");  // Insert selected command, exit slash popup mode.
  pty.send_keys("\r");  // Execute the slash command.

  // Wait for the response to be received (with timeout).
  auto start = std::chrono::steady_clock::now();
  while (!response_received->load(std::memory_order_acquire)) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count();
    if (elapsed >= 10000) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  REQUIRE(response_received->load(std::memory_order_acquire));
  REQUIRE(last_response->find("Available commands") != std::string::npos);

  // Quit.
  pty.send_keys("q");
  REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));
  app_thread.join();
  guard.joined = true;
  pty.restore();
}

// ---------------------------------------------------------------------------
// Test 11: Unknown command — /nonexistent returns error text.
// ---------------------------------------------------------------------------
TEST_CASE("SlashCommand PTY: unknown command shows error")
{
  pty_test::pty_terminal pty;
  std::atomic<bool> app_exited{false};
  pty_test::pty_drainer drainer(pty.fd());
  auto response_received = std::make_shared<std::atomic<bool>>(false);
  auto last_response = std::make_shared<std::string>();

  std::thread app_thread([&]() {
    using namespace cpptui;
    try {
      App app;
      test_exec_app ta;
      ta.response_received = response_received;
      ta.last_response = last_response;
      ta.build(app);
    } catch (const std::exception& e) {
      std::cerr << "[test11] exception: " << e.what() << "\n";
    } catch (...) {
      std::cerr << "[test11] unknown exception\n";
    }
    app_exited.store(true, std::memory_order_release);
  });

  pty_test::app_cleanup guard{pty, app_exited, app_thread};

  REQUIRE(drainer.wait_for("READY", 5000));

  // Type '/nonexistent' then Enter to insert, then Enter to execute.
  // Since /nonexistent is not in the popup, the first Enter inserts the
  // first matching command (or the typed text if no match). We need to
  // type the full text and use double-Enter to execute.
  pty.send_keys("/nonexistent");
  pty.send_keys("\r");  // Insert from popup (or typed text).
  pty.send_keys("\r");  // Execute the slash command.

  // Wait for the response to be received (with timeout).
  auto start = std::chrono::steady_clock::now();
  while (!response_received->load(std::memory_order_acquire)) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count();
    if (elapsed >= 10000) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  REQUIRE(response_received->load(std::memory_order_acquire));
  REQUIRE(last_response->find("Unknown command") != std::string::npos);

  // Quit.
  pty.send_keys("q");
  REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));
  app_thread.join();
  guard.joined = true;
  pty.restore();
}

// ---------------------------------------------------------------------------
// Test 12: Agent-busy guard — slash command not sent when agent is busy.
// ---------------------------------------------------------------------------
TEST_CASE("SlashCommand PTY: agent-busy guard blocks execution")
{
  pty_test::pty_terminal pty;
  std::atomic<bool> app_exited{false};
  pty_test::pty_drainer drainer(pty.fd());
  auto response_received = std::make_shared<std::atomic<bool>>(false);

  std::thread app_thread([&]() {
    using namespace cpptui;
    try {
      App app;
      test_exec_app ta;
      ta.response_received = response_received;
      ta.agent_busy.store(true, std::memory_order_release);
      ta.build(app);
    } catch (const std::exception& e) {
      std::cerr << "[test12] exception: " << e.what() << "\n";
    } catch (...) {
      std::cerr << "[test12] unknown exception\n";
    }
    app_exited.store(true, std::memory_order_release);
  });

  pty_test::app_cleanup guard{pty, app_exited, app_thread};

  REQUIRE(drainer.wait_for("READY", 5000));

  // Type '/help' then double-Enter while agent is busy.
  pty.send_keys("/help");
  pty.send_keys("\r");  // Insert from popup.
  pty.send_keys("\r");  // Execute — should be blocked by agent-busy guard.

  // Wait a bit — the response should NOT appear since agent is busy.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  REQUIRE(!response_received->load(std::memory_order_acquire));

  // Quit.
  pty.send_keys("q");
  REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));
  app_thread.join();
  guard.joined = true;
  pty.restore();
}

// ---------------------------------------------------------------------------
// Test 13: /exit quits the application.
// ---------------------------------------------------------------------------
TEST_CASE("SlashCommand PTY: /exit quits the app")
{
  pty_test::pty_terminal pty;
  std::atomic<bool> app_exited{false};
  pty_test::pty_drainer drainer(pty.fd());

  std::thread app_thread([&]() {
    using namespace cpptui;
    try {
      App app;
      test_exec_app ta;
      ta.build(app);
    } catch (const std::exception& e) {
      std::cerr << "[test13] exception: " << e.what() << "\n";
    } catch (...) {
      std::cerr << "[test13] unknown exception\n";
    }
    app_exited.store(true, std::memory_order_release);
  });

  pty_test::app_cleanup guard{pty, app_exited, app_thread};

  REQUIRE(drainer.wait_for("READY", 5000));

  // Type '/exit' then Enter to insert, then Enter to execute.
  pty.send_keys("/exit");
  pty.send_keys("\r");  // Insert from popup.
  pty.send_keys("\r");  // Execute — calls app.quit().

  // Verify the app exits.
  REQUIRE(pty_test::drain_wait(app_exited, pty, 5000));
  app_thread.join();
  guard.joined = true;
  pty.restore();
}