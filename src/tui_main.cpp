#include "http/tls_connection.h"
#include "http/http_coordinator_task.h"
#include "api/mistral_types.h"
#include "agent/agent_system.h"
#include "agent/agent_system_task.h"
#include "agent/agent_system_stats_task.h"
#include "agent/agent_config.h"
#include "tools/note_tools.h"
#include "template/template_engine.h"
#include "trace/trace.h"
#include "common/signal_watcher_task.h"
#include "agent/interactive_agent_task.h"
#include "tui_render_sink.h"

#include <dripcore/scheduler.h>
#include <dripcore/channel.h>
#include <dripcore/task.h>
#include <uri.h>

#include "cpptui.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <functional>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Global state for SIGINT → dripcore shutdown.
// ---------------------------------------------------------------------------
static int g_sigint_pipe_write_fd = -1;

static void sigint_handler(int)
{
  if (g_sigint_pipe_write_fd >= 0) {
    char buf = '1';
    ssize_t ret = ::write(g_sigint_pipe_write_fd, &buf, 1);
    (void)ret;
  }
}

// ---------------------------------------------------------------------------
// Thread-safe approval prompt queue: dripcore thread → TUI thread.
// The prompt_bridge_task reads from prompt_ch_ and pushes approval
// prompts here. The TUI timer checks this queue and shows a modal dialog.
// ---------------------------------------------------------------------------
struct approval_queue
{
  std::mutex mtx;
  std::optional<std::string> pending_prompt;
  std::atomic<bool> has_data{false};

  void push(std::string prompt)
  {
    {
      std::lock_guard<std::mutex> lock(mtx);
      pending_prompt.emplace(std::move(prompt));
    }
    has_data.store(true, std::memory_order_release);
  }

  std::optional<std::string> drain()
  {
    std::lock_guard<std::mutex> lock(mtx);
    std::optional<std::string> result;
    if (pending_prompt) {
      result = std::move(*pending_prompt);
      pending_prompt.reset();
    }
    has_data.store(false, std::memory_order_release);
    return result;
  }
};

// ---------------------------------------------------------------------------
// prompt_bridge_task: runs in dripcore context, bridges prompt_ch_ to the
// TUI thread. Reads prompt messages from the agent task and routes them:
//   - "mistral" → sets agent_ready flag (agent is ready for input)
//   - "[APPROVAL_PROMPT]..." → pushes to approval_queue for modal display
//   - Other (e.g. "Enter message...") → pushes to approval_queue as well
//
// This is necessary because dripcore channels can only be consumed from
// within a dripcore task context.
// ---------------------------------------------------------------------------
class prompt_bridge_task : public dripcore::task
{
public:
  prompt_bridge_task(
    std::shared_ptr<dripcore::channel<std::string>> prompt_ch,
    std::atomic<bool>& agent_ready,
    approval_queue& approval_q
  )
    : prompt_ch_(std::move(prompt_ch))
    , agent_ready_(agent_ready)
    , approval_q_(approval_q)
  {
  }

  dripcore::awaitable<> body() override
  {
    while (true)
    {
      auto msg = co_await prompt_ch_->recv(this);
      if (!msg) break;  // Channel closed.

      std::cerr << "[prompt_bridge] received: " << msg->substr(0, 80) << std::endl;

      if (msg->find("[APPROVAL_PROMPT]") == 0)
      {
        approval_q_.push(msg->substr(std::string("[APPROVAL_PROMPT]").length()));
      }
      else if (*msg == "mistral")
      {
        agent_ready_.store(true, std::memory_order_release);
      }
      else
      {
        // Other prompts (e.g. "Enter message for AI...") — treat as
        // approval-style prompts that need text input.
        approval_q_.push(*msg);
      }
    }

    std::cerr << "[prompt_bridge] channel closed, exiting" << std::endl;
    co_return;
  }

private:
  std::shared_ptr<dripcore::channel<std::string>> prompt_ch_;
  std::atomic<bool>& agent_ready_;
  approval_queue& approval_q_;
};

// ---------------------------------------------------------------------------
// Thread-safe input queue: TUI thread → dripcore thread.
// The TUI pushes user messages here and writes a wakeup byte to a pipe.
// The input_bridge_task (running in dripcore context) reads the pipe,
// drains this queue, and sends to the agent via dripcore::channel.
// ---------------------------------------------------------------------------
struct input_queue
{
  std::mutex mtx;
  std::deque<std::string> messages;

  void push(std::string msg)
  {
    std::lock_guard<std::mutex> lock(mtx);
    messages.push_back(std::move(msg));
  }

  std::deque<std::string> drain()
  {
    std::lock_guard<std::mutex> lock(mtx);
    std::deque<std::string> result;
    result.swap(messages);
    return result;
  }
};

// ---------------------------------------------------------------------------
// input_bridge_task: runs in dripcore context, bridges TUI input to the
// agent's dripcore::channel. This is necessary because dripcore channels
// can only be used from within a dripcore task context (they call
// executor::current() internally).
//
// Protocol:
//   Pipe readable → drain input_queue, send each message to input_ch
//   Pipe EOF (write end closed) → close input_ch, exit
// ---------------------------------------------------------------------------
class input_bridge_task : public dripcore::task
{
public:
  input_bridge_task(
    int pipe_read_fd,
    std::shared_ptr<dripcore::channel<std::string>> input_ch,
    input_queue& queue
  )
    : pipe_read_fd_(pipe_read_fd)
    , input_ch_(std::move(input_ch))
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
        // EOF — pipe write end closed, shutdown.
        input_ch_->close();
        co_return;
      }

      // Drain all pending messages and send to agent.
      auto messages = queue_.drain();
      for (auto& msg : messages) {
        input_ch_->try_send(std::move(msg));
      }
    }

    co_return;
  }

private:
  int pipe_read_fd_;
  std::shared_ptr<dripcore::channel<std::string>> input_ch_;
  input_queue& queue_;
};

// ---------------------------------------------------------------------------
// Divider: renders a horizontal line across full width.
// ---------------------------------------------------------------------------
class Divider : public cpptui::Widget {
public:
  Divider() { fixed_height = 1; }

  void render(cpptui::Buffer& buffer) override {
    cpptui::Color fg = fg_color.resolve(cpptui::Theme::current().border);
    cpptui::Color bg = bg_color.resolve(cpptui::Theme::current().background);
    for (int i = 0; i < width; ++i) {
      cpptui::Cell c;
      c.content = "\xe2\x94\x80";  // ─ U+2500
      c.fg_color = fg;
      c.bg_color = bg;
      buffer.set(x + i, y, c);
    }
  }
};

// ---------------------------------------------------------------------------
// FooterBar: renders three strings at left, center, and right positions.
// ---------------------------------------------------------------------------
class FooterBar : public cpptui::Widget {
public:
  FooterBar() { fixed_height = 1; }

  std::string left_text;
  std::string center_text;
  std::string right_text;

  void render(cpptui::Buffer& buffer) override {
    cpptui::Color fg = fg_color.resolve(cpptui::Theme::current().panel_fg);
    cpptui::Color bg = bg_color.resolve(cpptui::Theme::current().panel_bg);

    // Fill background
    for (int i = 0; i < width; ++i) {
      cpptui::Cell c;
      c.content = " ";
      c.fg_color = fg;
      c.bg_color = bg;
      buffer.set(x + i, y, c);
    }

    auto render_text = [&](const std::string& text, int start_x) {
      auto chars = cpptui::TextHelper::prepare_text_for_render(text);
      int dx = 0;
      for (const auto& ci : chars) {
        if (start_x + dx >= width) break;
        cpptui::Cell c;
        c.content = ci.content;
        c.fg_color = fg;
        c.bg_color = bg;
        buffer.set(x + start_x + dx, y, c);
        if (ci.display_width == 2 && start_x + dx + 1 < width) {
          cpptui::Cell skip;
          skip.content = "";
          skip.bg_color = bg;
          buffer.set(x + start_x + dx + 1, y, skip);
        }
        dx += ci.display_width;
      }
    };

    // Left (1px left margin)
    render_text(left_text, 1);

    // Center (within inner width, offset by 1px left margin)
    int inner_w = width - 2;
    auto center_chars = cpptui::TextHelper::prepare_text_for_render(center_text);
    int center_w = 0;
    for (const auto& ci : center_chars) center_w += ci.display_width;
    render_text(center_text, 1 + (inner_w - center_w) / 2);

    // Right (1px right margin)
    auto right_chars = cpptui::TextHelper::prepare_text_for_render(right_text);
    int right_w = 0;
    for (const auto& ci : right_chars) right_w += ci.display_width;
    render_text(right_text, width - 1 - right_w);
  }
};

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[])
{
  // Default configuration.
  std::string base_dir    = ".mistral-cli";
  std::string notes_dir   = ".mistral-cli/notes";
  std::string host        = "api.mistral.ai";
  int port                = 443;
  std::string agent_id    = "main";
  std::string config_file_path;
  std::string theme_name  = "dark";

  // Parse command-line arguments (minimal for prototype).
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-a" && i + 1 < argc) { agent_id = argv[++i]; }
    else if (arg == "-H" && i + 1 < argc) { host = argv[++i]; }
    else if (arg == "-p" && i + 1 < argc) { port = std::atoi(argv[++i]); }
    else if (arg == "-c" && i + 1 < argc) { config_file_path = argv[++i]; }
    else if (arg == "--theme" && i + 1 < argc) { theme_name = argv[++i]; }
    else if (arg == "-h") {
      std::cout << "Usage: mistral-tui [OPTIONS]\n"
                << "  -c FILE       Config file\n"
                << "  -H HOST       API host\n"
                << "  -p PORT       API port\n"
                << "  -a ID         Agent ID\n"
                << "  --theme NAME  Theme: dark, light, nord, tokyo-night, solarized-light\n";
      return 0;
    }
  }

  if (config_file_path.empty()) {
    config_file_path = std::filesystem::path(base_dir) / "mistral-cli.json";
  }

  // Initialize trace logger.
  auto log_filename = std::filesystem::path(base_dir) / "trace.log";
  mistral::trace::logger::instance().set_log_path(log_filename.string());
  mistral::trace::logger::instance().enable(false);

  // Initialize OpenSSL.
  mistral::net::init_openssl();

  // Create channels.
  auto prompt_ch        = std::make_shared<dripcore::channel<std::string>>(4);
  auto input_ch         = std::make_shared<dripcore::channel<std::string>>(1);
  auto request_ch       = std::make_shared<dripcore::channel<mistral::api::api_request>>(4);
  auto agent_response_ch = std::make_shared<dripcore::channel<std::string>>(32);

  // Initialize agent system.
  auto& agent_system = mistral::agent::agent_system::instance();
  std::filesystem::path config_file(config_file_path);

  if (!std::filesystem::exists(config_file)) {
    std::cerr << "configuration file '" << config_file << "' not found" << std::endl;
    return 1;
  }

  try {
    agent_system.initialize(config_file.string(), base_dir, notes_dir, request_ch);
  } catch (const std::exception& e) {
    std::cerr << "error loading configuration: " << e.what() << std::endl;
    return 1;
  }

  if (!agent_system.has_agent_config(agent_id)) {
    std::cerr << "error: agent '" << agent_id << "' not found" << std::endl;
    return 1;
  }

  const auto& agent_config = agent_system.get_agent_config(agent_id);
  const auto& model_config = agent_system.get_model_config(agent_config.model);

  // Resolve host/port from config.
  if (host == "api.mistral.ai" && !model_config.base_url.empty()) {
    auto parse_result = uri::parse(model_config.base_url);
    if (std::holds_alternative<uri::uri_view>(parse_result)) {
      const auto& parsed_uri = std::get<uri::uri_view>(parse_result);
      host = parsed_uri.host();
      auto port_str = parsed_uri.port();
      if (!port_str.empty()) {
        try { port = std::stoi(std::string(port_str)); } catch (...) {}
      }
    }
  }

  // Read API key.
  const char* api_key_env = std::getenv(model_config.api_key_env.c_str());
  if (!api_key_env) {
    std::cerr << "error: " << model_config.api_key_env << " not set" << std::endl;
    return 1;
  }
  std::string api_key(api_key_env);

  std::string resolved_model = model_config.model_id;

  // -----------------------------------------------------------------------
  // Set up SIGINT self-pipe for dripcore.
  // -----------------------------------------------------------------------
  int sigint_pipe_fds[2];
  if (::pipe(sigint_pipe_fds) != 0) {
    std::cerr << "error: failed to create signal pipe" << std::endl;
    return 1;
  }
  g_sigint_pipe_write_fd = sigint_pipe_fds[1];
  agent_system.set_shutdown_pipe_fd(sigint_pipe_fds[1]);

  struct sigaction sa{};
  sa.sa_handler = sigint_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);

  // -----------------------------------------------------------------------
  // Set up input pipe + queue for TUI → dripcore communication.
  // -----------------------------------------------------------------------
  int input_pipe_fds[2];
  if (::pipe(input_pipe_fds) != 0) {
    std::cerr << "error: failed to create input pipe" << std::endl;
    return 1;
  }
  input_queue inq;

  // -----------------------------------------------------------------------
  // Agent-ready flag.
  // -----------------------------------------------------------------------
  std::atomic<bool> agent_ready{true};
  approval_queue approval_q;
  std::atomic<bool> approval_pending{false};

  // Redirect stderr to a log file — the HTTP coordinator and other
  // dripcore tasks write diagnostic messages to std::cerr which would
  // corrupt the TUI's alternate screen buffer. Write to a log file
  // instead so errors can be diagnosed without breaking the TUI.
  // This is done after all startup validation so fatal errors print
  // to the terminal.
  std::freopen(".mistral-cli/tui-stderr.log", "w", stderr);

  // -----------------------------------------------------------------------
  // Build the TUI.
  // -----------------------------------------------------------------------
  using namespace cpptui;

  App app;
  if (theme_name == "light") {
    Theme::set_theme(Theme::Light());
  } else if (theme_name == "nord") {
    Theme::set_theme(Theme::Nord());
  } else if (theme_name == "tokyo-night") {
    Theme::set_theme(Theme::TokyoNight());
  } else if (theme_name == "solarized-light") {
    Theme::set_theme(Theme::SolarizedLight());
  } else {
    Theme::set_theme(Theme::Dark());
  }

  // Header: connectivity dot to the right.
  auto dot = std::make_shared<Label>(
    StyledText("\xe2\x97\x8f")  // ● U+25CF
  );
  dot->fixed_width = 1;
  dot->focusable = false;
  dot->fg_color = Theme::current().border;  // dim until agent is ready

  auto header = std::make_shared<Horizontal>();
  header->fixed_height = 1;
  header->add(std::make_shared<HorizontalSpacer>(1));
  header->add(std::make_shared<HorizontalSpacer>());
  header->add(dot);
  header->add(std::make_shared<HorizontalSpacer>(1));

  // Content area (scrollable)
  auto content = std::make_shared<Paragraph>();
  content->set_text("mistral-tui prototype\nType your message and press Enter. Ctrl+C to quit.\n\n");
  content->word_wrap = true;
  content->fg_color = Theme::current().foreground;
  content->bg_color = Theme::current().background;

  // Set initial fixed_height based on wrapped line count so that
  // ScrollableVertical::layout() computes the correct content_height
  // (distribute_flex treats fixed_height children as non-flex).
  {
    int w = content->width > 0 ? content->width : 80;
    auto lines = Paragraph::wrap_text_static(
      content->text, w, content->first_line_indent,
      content->hanging_indent, content->preserve_whitespace);
    content->fixed_height = (int)lines.size();
  }

  auto scroll = std::make_shared<ScrollableVertical>();
  scroll->add(content);
  // Don't let the scroll container steal focus from the input field.
  scroll->focusable = false;

  // Input field (3 lines)
  auto input = std::make_shared<Input>();
  input->placeholder = "Type your message...";
  input->fixed_height = 3;

  // Footer: workspace (left), agent · model (center), usage (right)
  auto footer = std::make_shared<FooterBar>();
  footer->left_text = std::filesystem::current_path().string();
  footer->center_text = agent_id + " \xc2\xb7 " + resolved_model;
  footer->right_text = "0 tokens";
  footer->fg_color = Theme::current().panel_fg;
  footer->bg_color = Theme::current().panel_bg;

  // Layout: header (1) | divider (1) | content (flex) | divider (1) | input (3) | divider (1) | footer (1)
  auto root = std::make_shared<Vertical>();
  // Give root and input temporary non-zero dimensions so that
  // App::run()'s initial collect_focusables() finds the Input widget.
  // The real dimensions are set during the first render/layout pass.
  root->width = 80;
  root->height = 24;
  input->width = 80;
  input->height = 3;

  root->add(header);
  root->add(std::make_shared<Divider>());
  root->add(scroll);
  root->add(std::make_shared<Divider>());
  root->add(input);
  root->add(std::make_shared<Divider>());
  root->add(footer);

  // -----------------------------------------------------------------------
  // Content append helper: appends text to the content widget, recalculates
  // fixed_height for ScrollableVertical, and auto-scrolls to bottom if the
  // user was already at the bottom.
  //
  // Called directly from the UI thread (e.g. user input echo) and from
  // tui_render_sink via App::post() (agent output streaming).
  // -----------------------------------------------------------------------
  std::function<void(const std::string&)> append_content =
    [content, scroll](const std::string& fragment) {
      // Check if user is at the bottom before adding content.
      int visible_h = scroll->height > 0 ? scroll->height : 1;
      int ms = scroll->content_height - visible_h;
      bool was_at_bottom = (scroll->scroll_offset >= (ms < 0 ? 0 : ms));

      // Append to content widget.
      content->set_text(content->text + fragment);

      // Recalculate fixed_height from wrapped line count so that
      // ScrollableVertical::layout() sets content_height correctly.
      int wrap_width = content->width > 0 ? content->width : 80;
      auto lines = cpptui::Paragraph::wrap_text_static(
        content->text, wrap_width, content->first_line_indent,
        content->hanging_indent, content->preserve_whitespace);
      content->fixed_height = (int)lines.size();

      // Auto-scroll to bottom only if the user was at the bottom.
      if (was_at_bottom) {
        int max_scroll = (int)lines.size() - visible_h;
        if (max_scroll < 0) max_scroll = 0;
        scroll->scroll_offset = max_scroll;
      }
    };

  // -----------------------------------------------------------------------
  // Wire up input: Enter sends message to agent via pipe + queue.
  // We cannot call dripcore::channel::try_send() from the TUI thread
  // because it calls executor::current() which returns nullptr outside
  // a dripcore task context. Instead, we push to a thread-safe queue
  // and write a wakeup byte to a pipe. The input_bridge_task (running
  // in dripcore) reads the pipe and sends to the channel.
  // -----------------------------------------------------------------------
  auto send_input = [&]() {
    // If an approval dialog is open, Enter should not send a chat message.
    if (approval_pending.load(std::memory_order_acquire)) return;

    std::string text = input->get_value();
    if (text.empty()) return;

    // Append user message to content.
    append_content("\n> " + text + "\n");

    // Clear input field.
    input->set_value("");

    // Push to thread-safe queue and wake up the bridge task.
    inq.push(std::move(text));
    char wakeup = '1';
    ::write(input_pipe_fds[1], &wakeup, 1);
  };

  // Register both key 10 (LF) and key 13 (CR) for Enter.
  // Terminals typically send CR (13) for Enter, but some send LF (10).
  app.register_key(10, send_input);
  app.register_key(13, send_input);

  // -----------------------------------------------------------------------
  // Approval dialog: shown when the agent requests tool approval.
  // -----------------------------------------------------------------------
  auto approval_dialog = std::make_shared<Dialog>(&app, BorderStyle::Single);
  approval_dialog->modal = true;
  approval_dialog->fixed_width = 60;
  approval_dialog->fixed_height = 10;

  auto approval_text = std::make_shared<Paragraph>();
  approval_text->word_wrap = true;
  approval_text->fg_color = Theme::current().foreground;
  approval_text->bg_color = Theme::current().panel_bg;
  approval_dialog->add(approval_text);

  // Helper: send a single-char approval response and close the dialog.
  auto send_approval_response = [&](char response) {
    approval_pending.store(false, std::memory_order_release);
    app.close_dialog(approval_dialog);

    // Unregister approval key handlers.
    for (int k : {'y', 'n', 'r', 'a', '?'}) {
      app.unregister_key(k);
    }

    // Send the response through the input queue + pipe wakeup.
    inq.push(std::string(1, response));
    char wakeup = '1';
    ::write(input_pipe_fds[1], &wakeup, 1);
  };

  // Register approval key handlers (registered on demand when dialog opens).
  auto register_approval_keys = [&]() {
    app.register_key('y', [&]() { send_approval_response('y'); });
    app.register_key('n', [&]() { send_approval_response('n'); });
    app.register_key('r', [&]() { send_approval_response('r'); });
    app.register_key('a', [&]() { send_approval_response('a'); });
    app.register_key('?', [&]() { send_approval_response('?'); });
  };

  // -----------------------------------------------------------------------
  // Auto-scroll logic: before new content arrives, check if the user is
  // at the bottom. If so, auto-scroll to the new bottom. If not (the user
  // scrolled up via mouse wheel or keyboard), preserve their position.
  // This works for both mouse wheel (handled by ScrollableVertical) and
  // keyboard scrolling (registered below), since both modify scroll_offset
  // directly.
  // -----------------------------------------------------------------------

  // Helper: compute max scroll offset.
  auto max_scroll_offset = [&]() -> int {
    int visible_h = scroll->height > 0 ? scroll->height : 1;
    int ms = scroll->content_height - visible_h;
    return ms < 0 ? 0 : ms;
  };

  // Helper: clamp scroll_offset to valid range.
  auto clamp_scroll = [&]() {
    if (scroll->scroll_offset < 0) scroll->scroll_offset = 0;
    int ms = max_scroll_offset();
    if (scroll->scroll_offset > ms) scroll->scroll_offset = ms;
  };

  // PageUp: scroll up by one viewport height.
  app.register_key(1001, [&]() {
    int visible_h = scroll->height > 0 ? scroll->height : 1;
    scroll->scroll_offset -= visible_h;
    clamp_scroll();
  });

  // PageDown: scroll down by one viewport height.
  app.register_key(1002, [&]() {
    int visible_h = scroll->height > 0 ? scroll->height : 1;
    scroll->scroll_offset += visible_h;
    clamp_scroll();
  });

  // Shift+Up: scroll up one line.
  app.register_key(1065, [&]() {
    scroll->scroll_offset -= 1;
    clamp_scroll();
  }, false, true);

  // Shift+Down: scroll down one line.
  app.register_key(1066, [&]() {
    scroll->scroll_offset += 1;
    clamp_scroll();
  }, false, true);

  // -----------------------------------------------------------------------
  // Timer: drain output queue and update content widget.
  // -----------------------------------------------------------------------
  // -----------------------------------------------------------------------
  // Timer: check for pending approval prompts and show dialog.
  // -----------------------------------------------------------------------
  app.add_timer(50, [&]() {
    if (!approval_q.has_data.load(std::memory_order_acquire)) return;

    auto prompt = approval_q.drain();
    if (!prompt) return;

    // Show the approval dialog with the prompt text.
    approval_text->set_text(*prompt + "\n\n"
      "[y] Approve  [n] Reject  [r] Reject+msg  [a] Approve All  [?] Help");
    approval_text->fixed_height = 0;  // auto-size

    // Set width/height from fixed_ values — Dialog::show() calls layout()
    // which uses width/height (not fixed_width/fixed_height).
    approval_dialog->width = approval_dialog->fixed_width;
    approval_dialog->height = approval_dialog->fixed_height;

    approval_pending.store(true, std::memory_order_release);
    register_approval_keys();

    std::cerr << "[tui] opening approval dialog" << std::endl;
    app.open_dialog(approval_dialog);
  });

  // -----------------------------------------------------------------------
  // Timer: update connectivity dot and footer usage stats.
  // -----------------------------------------------------------------------
  app.add_timer(200, [&]() {
    // Update connectivity dot
    if (agent_ready.load(std::memory_order_acquire)) {
      dot->fg_color = Theme::current().success;
    } else {
      dot->fg_color = Theme::current().border;
    }

    // Update footer usage
    auto stats = agent_system.get_usage_stats();
    if (stats.contains("total_tokens")) {
      footer->right_text =
        std::to_string(stats["total_tokens"].get<int>()) + " tokens";
    }
  });

  // -----------------------------------------------------------------------
  // Ctrl+C to quit.
  // cpp-tui handles Ctrl+C internally as a mandatory global exit —
  // it sets running=false and breaks out of the event loop.
  // No register_exit_key needed.
  // -----------------------------------------------------------------------

  // -----------------------------------------------------------------------
  // Launch dripcore scheduler in a background thread.
  // -----------------------------------------------------------------------
  std::thread dripcore_thread([&]() {
    dripcore::scheduler sched;

    // Spawn agent_system_task to capture responses.
    sched.spawn<mistral::agent::agent_system_task>(agent_response_ch);

    // Spawn agent_system_stats_task.
    sched.spawn<mistral::agent::agent_system_stats_task>(agent_system.stats_ch());

    // Spawn HTTP coordinator.
    sched.spawn<mistral::net::http_coordinator_task>(host, port, api_key, request_ch);

    // Spawn input bridge task (TUI → dripcore).
    sched.spawn<input_bridge_task>(input_pipe_fds[0], input_ch, std::ref(inq));

    // Spawn prompt bridge task (dripcore → TUI).
    sched.spawn<prompt_bridge_task>(prompt_ch, std::ref(agent_ready), std::ref(approval_q));

    auto sink = std::make_shared<mistral::agent::tui_render_sink>(
      [&app](std::function<void()> cb) { app.post(std::move(cb)); },
      append_content
    );
    sched.spawn<mistral::agent::interactive_agent_task>(
      agent_id, input_ch, agent_response_ch, prompt_ch, sink
    );

    // Spawn signal watcher for SIGINT.
    sched.spawn<mistral::common::signal_watcher_task>(sigint_pipe_fds[0]);

    sched.run();
  });

  // -----------------------------------------------------------------------
  // Run the TUI event loop (blocks until quit).
  // -----------------------------------------------------------------------
  app.run(root);

  // -----------------------------------------------------------------------
  // Shutdown: signal dripcore to stop and wait for it.
  //
  // 1. Write '1' to SIGINT pipe → signal_watcher_task cancels any
  //    in-flight API request by closing the response channel.
  //
  // 2. Close the input pipe write end → input_bridge_task gets EOF →
  //    closes input_ch → interactive_agent_task sees closed channel and exits →
  //    interactive_agent_task::teardown() closes request_ch, output_ch, stats_ch,
  //    and calls signal_shutdown() → cascade shutdown of all remaining
  //    dripcore tasks.
  //
  // 3. Join the dripcore thread (sched.run() returns when all tasks
  //    are done).
  // -----------------------------------------------------------------------
  char cancel_byte = '1';
  ::write(g_sigint_pipe_write_fd, &cancel_byte, 1);

  ::close(input_pipe_fds[1]);

  // Wait for dripcore thread to finish.
  if (dripcore_thread.joinable()) {
    dripcore_thread.join();
  }

  // Restore signal handler and close pipes.
  signal(SIGINT, SIG_DFL);
  ::close(input_pipe_fds[0]);
  ::close(sigint_pipe_fds[0]);
  ::close(sigint_pipe_fds[1]);

  return 0;
}