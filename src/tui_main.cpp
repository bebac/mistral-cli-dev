#include "http/tls_connection.h"
#include "http/http_coordinator_task.h"
#include "api/mistral_types.h"
#include "agent/agent_system.h"
#include "agent/agent_system_task.h"
#include "agent/agent_system_stats_task.h"
#include "agent/agent_config.h"
#include "agent/interactive_agent_task.h"
#include "tools/note_tools.h"
#include "template/template_engine.h"
#include "trace/trace.h"
#include "common/signal_watcher_task.h"

#include <dripcore/scheduler.h>
#include <dripcore/channel.h>
#include <dripcore/task.h>
#include <uri.h>

#include "cpptui.hpp"
#include "tui/tui_render_sink.h"
#include "tui/tui_widgets.h"

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
// prompt_bridge_task: runs in dripcore context, bridges prompt_ch_ to the
// TUI thread. Reads prompt messages from the agent task and routes them:
//   - "mistral" → sets agent_ready flag (agent is ready for input)
//   - "[APPROVAL_PROMPT]..." → appends to content area via App::post,
//     sets approval mode to single_key
//   - Other (e.g. "Enter message for AI...") → appends to content area
//     via App::post, sets approval mode to text_input
//
// This is necessary because dripcore channels can only be consumed from
// within a dripcore task context.
// ---------------------------------------------------------------------------
class prompt_bridge_task : public dripcore::task
{
public:
  using post_fn = std::function<void(std::function<void()>)>;
  using append_fn = std::function<void(const std::string&, mistral::agent::content_type)>;

  prompt_bridge_task(
    std::shared_ptr<dripcore::channel<std::string>> prompt_ch,
    std::atomic<bool>& agent_ready,
    std::atomic<int>& approval_state,
    post_fn post,
    append_fn append_content
  )
    : prompt_ch_(std::move(prompt_ch))
    , agent_ready_(agent_ready)
    , approval_state_(approval_state)
    , post_(std::move(post))
    , append_content_(std::move(append_content))
  {
  }

  dripcore::awaitable<> body() override
  {
    while (true)
    {
      auto msg = co_await prompt_ch_->recv(this);
      if (!msg) break;  // Channel closed.

      mistral::trace::logger::instance().trace(
        mistral::trace::category::approval,
        "prompt_bridge received: {}", msg->substr(0, 80)
      );

      if (msg->find("[APPROVAL_PROMPT]") == 0)
      {
        std::string prompt_text =
          msg->substr(std::string("[APPROVAL_PROMPT]").length());
        approval_state_.store(1, std::memory_order_release);
        post_([fn = append_content_, text = std::move(prompt_text)]() {
          fn(text, mistral::agent::content_type::system);
        });
      }
      else if (*msg == "mistral")
      {
        agent_ready_.store(true, std::memory_order_release);
      }
      else
      {
        // Other prompts (e.g. "Enter message for AI...") — append inline.
        // These are text-input prompts from the reject-with-message flow.
        // The agent task already sent the [APPROVAL_PROMPT] first, which
        // set approval_state to 1 (single_key). The user pressed 'r',
        // which set approval_state to 2 (text_input) and sent 'r' back.
        // Now the agent sends the text prompt. We just append it inline.
        post_([fn = append_content_, text = *msg]() {
          fn(text, mistral::agent::content_type::system);
        });
      }
    }

    mistral::trace::logger::instance().trace(
      mistral::trace::category::approval,
      "prompt_bridge channel closed, exiting"
    );
    co_return;
  }

private:
  std::shared_ptr<dripcore::channel<std::string>> prompt_ch_;
  std::atomic<bool>& agent_ready_;
  std::atomic<int>& approval_state_;
  post_fn post_;
  append_fn append_content_;
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
  std::atomic<int> approval_state{0};  // 0=none, 1=single_key, 2=text_input

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
  content->word_wrap = true;
  content->preserve_whitespace = true;
  content->fg_color = Theme::current().foreground;
  content->bg_color = Theme::current().background;

  // Styled content accumulator: spans are appended with appropriate
  // colors based on content_type. This replaces plain text mode.
  StyledText content_styled;
  content_styled.add("mistral-tui prototype\nType your message. Alt+Enter or double-Enter to send. ESC to cancel. Ctrl+C to quit.\n\n");
  content->set_styled(content_styled);

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

  // Input area: multiline TextArea (5 lines), wrapped in a Horizontal
  // with 1-char spacers on each side for symmetric margins.
  auto input_row = std::make_shared<Horizontal>();
  input_row->fixed_height = 5;

  auto input = std::make_shared<ChatInput>();
  input->show_line_numbers = false;
  input->word_wrap = true;
  input->show_scrollbar = true;
  input->accepts_tab = false;
  input->fixed_height = 5;

  input_row->add(std::make_shared<HorizontalSpacer>(1));
  input_row->add(input);
  //input_row->add(std::make_shared<HorizontalSpacer>(1)); // Scrollbar adds 1 character.

  // Input hint: 1-line widget for approval key bindings.
  // Empty in normal chat mode; shows hints when approval is active.
  auto input_hint = std::make_shared<InputHint>();
  input_hint->fixed_height = 1;

  // Divider above hint (turns amber in approval mode).
  // Hidden in normal mode, shown only during approval.
  auto input_divider = std::make_shared<Divider>();
  input_divider->visible = false;

  // Divider above input, below hint (turns amber in approval mode).
  auto input_divider_top = std::make_shared<Divider>();

  // Slash command popup: appears above input, grows upward.
  auto slash_divider = std::make_shared<Divider>();
  slash_divider->visible = false;

  auto slash_popup = std::make_shared<SlashCommandPopup>();
  slash_popup->fixed_height = 0;
  slash_popup->visible = false;
  slash_popup->set_registry(agent_system.slash_commands().get());

  // Hint line: hidden in normal mode, shown only during approval.
  input_hint->visible = false;

  // Footer: workspace (left), agent · model (center), usage (right)
  auto footer = std::make_shared<FooterBar>();
  footer->left_text = std::filesystem::current_path().string();
  footer->center_text = agent_id + " \xc2\xb7 " + resolved_model;
  footer->right_text = "0 tokens";
  footer->fg_color = Theme::current().panel_fg;
  footer->bg_color = Theme::current().panel_bg;

  // Layout: header (1) | divider (1) | content (flex) | divider (1) |
  //         hint (1) | divider (1) | input_row (5) | divider (1) | footer (1)
  auto root = std::make_shared<Vertical>();
  // Give root and input temporary non-zero dimensions so that
  // App::run()'s initial collect_focusables() finds the Input widget.
  // Without this, widgets with zero width/height are skipped during
  // focus collection, and the Input would not receive keyboard events
  // until the first render/layout pass sets real dimensions.
  // The real dimensions are set during the first render/layout pass.
  root->width = 80;
  root->height = 24;
  input_row->width = 80;
  input_row->height = 5;
  input->width = 78;
  input->height = 5;

  input_hint->width = 80;
  input_hint->height = 1;

  root->add(header);
  root->add(std::make_shared<Divider>());
  root->add(scroll);
  root->add(input_divider);
  root->add(input_hint);
  root->add(slash_divider);
  root->add(slash_popup);
  root->add(input_divider_top);
  root->add(input_row);
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
  // append_fragment: appends a styled span to content_styled. This is
  // O(1) and safe to call many times per batch without triggering the
  // expensive wrap_text_static re-wrap.
  auto append_fragment =
    [&content_styled](const std::string& fragment,
                      mistral::agent::content_type type) {
      using ct = mistral::agent::content_type;
      switch (type) {
        case ct::normal:
          content_styled.add(fragment);
          break;
        case ct::thinking:
          content_styled.colored(fragment, Theme::current().input_placeholder);
          break;
        case ct::tool_call:
          content_styled.colored_bold(fragment, Theme::current().primary);
          break;
        case ct::tool_result:
          content_styled.colored(fragment, Theme::current().primary);
          break;
        case ct::error:
          content_styled.colored_bold(fragment, Theme::current().error);
          break;
        case ct::warning:
          content_styled.colored(fragment, Theme::current().warning);
          break;
        case ct::user:
          content_styled.colored_bold(fragment, Theme::current().success);
          break;
        case ct::system:
          content_styled.colored(fragment, Theme::current().warning);
          break;
      }
    };

  // flush_content: performs the expensive O(n) work of setting styled
  // content, re-wrapping the entire text, and updating auto-scroll.
  // Called once per batch by tui_render_sink, or once per single
  // fragment by append_content (for non-batched callers).
  // Track whether the user is at the bottom of the scroll area.
  // Updated by flush_content, read by the approval timer to decide
  // whether to re-scroll after the hint/divider become visible.
  bool content_at_bottom = true;

  auto flush_content =
    [content, scroll, &content_styled, &content_at_bottom]() {
      // Check if user is at the bottom before adding content.
      int visible_h = scroll->height > 0 ? scroll->height : 1;
      int ms = scroll->content_height - visible_h;
      bool was_at_bottom = (scroll->scroll_offset >= (ms < 0 ? 0 : ms));
      content_at_bottom = was_at_bottom;

      content->set_styled(content_styled);

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

  // append_content: convenience wrapper for callers that post single
  // fragments (prompt_bridge_task, send_input). Calls append_fragment
  // then flush_content in one shot.
  std::function<void(const std::string&, mistral::agent::content_type)> append_content =
    [append_fragment, flush_content](const std::string& fragment,
                                     mistral::agent::content_type type) {
      append_fragment(fragment, type);
      flush_content();
    };

  // -----------------------------------------------------------------------
  // Wire up input: Alt+Enter or double-Enter sends message to agent via
  // pipe + queue. Plain Enter inserts a newline (handled by TextArea).
  // We cannot call dripcore::channel::try_send() from the TUI thread
  // because it calls executor::current() which returns nullptr outside
  // a dripcore task context. Instead, we push to a thread-safe queue
  // and write a wakeup byte to a pipe. The input_bridge_task (running
  // in dripcore) reads the pipe and sends to the channel.
  // -----------------------------------------------------------------------
  // Helper: send a response through the input queue + pipe wakeup.
  auto send_response = [&](std::string response) {
    inq.push(std::move(response));
    char wakeup = '1';
    ::write(input_pipe_fds[1], &wakeup, 1);
  };

  // Helper: exit approval mode, reset UI to normal chat.
  auto exit_approval_mode = [&]() {
    approval_state.store(0, std::memory_order_release);
    input->set_approval_mode(ChatInput::approval_mode::none);
    input_divider->visible = false;
    input_divider->override_fg = cpptui::Color();
    input_divider_top->override_fg = cpptui::Color();
    input_hint->visible = false;
    input_hint->text = "";
    input_hint->hint_fg = cpptui::Color();
    app.update();
  };

  // Normal chat submit: append user message and send to agent.
  auto send_input = [&]() {
    if (approval_state.load(std::memory_order_acquire) != 0) return;

    std::string text = input->get_text();
    if (text.empty()) return;

    // Append user message to content.
    append_content("\n> " + text + "\n", mistral::agent::content_type::user);

    // Clear input area.
    input->set_text("");

    // Mark agent as busy — connectivity dot dims until the agent
    // sends "mistral" (ready for next input) after completing or
    // being cancelled.
    agent_ready.store(false, std::memory_order_release);

    send_response(std::move(text));
  };

  // Single-key approval response: send char and return to normal mode.
  auto on_approval_key = [&](char ch) {
    char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

    if (lower == 'r') {
      // Send 'r' to the agent so it enters the REJECT_WITH_MESSAGE branch
      // and waits for the rejection message.
      send_response("r");

      // Transition to text_input mode for reject-with-message.
      approval_state.store(2, std::memory_order_release);
      input->set_approval_mode(ChatInput::approval_mode::text_input);
      input->set_text("");
      input_hint->text = "  Enter message for AI (or Enter for default):";
      input_hint->hint_fg = Theme::current().warning;
      input_divider_top->override_fg = Theme::current().warning;
      mistral::trace::logger::instance().trace(
        mistral::trace::category::approval,
        "approval: switching to text_input mode"
      );
      return;
    }

    // y, n, a, ? — send single char and return to normal mode.
    exit_approval_mode();
    send_response(std::string(1, lower));

    mistral::trace::logger::instance().trace(
      mistral::trace::category::approval,
      "approval: single-key response '{}'", lower
    );
  };

  // Text input approval submit: send rejection message and return to normal.
  auto on_approval_text_submit = [&]() {
    std::string text = input->get_text();
    exit_approval_mode();
    send_response(std::move(text));

    mistral::trace::logger::instance().trace(
      mistral::trace::category::approval,
      "approval: text response sent"
    );
  };

  // Wire callbacks to the ChatInput widget.
  input->on_submit = send_input;
  input->on_approval_key = on_approval_key;
  input->on_approval_text_submit = on_approval_text_submit;

  // Wire slash command mode.
  input->set_slash_popup(slash_popup.get());
  input->set_slash_divider(slash_divider.get());
  input->is_agent_busy = [&agent_ready]() {
    return !agent_ready.load(std::memory_order_acquire);
  };
  input->on_slash_command = [&](std::string command, std::string args) {
    if (command == "exit") {
      app.quit();
      return;
    }
    // Other commands: stub for now (task 2b handles execution).
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
  // ESC: cancel the current API request.
  //
  // Writes '1' to the SIGINT pipe. The signal_watcher_task (running in
  // dripcore) reads this and closes the current response channel via
  // agent_system::get_current_response_ch()->close(). The agent loop
  // sees the closed channel as a cancellation, calls
  // agent_loop_on_cancelled() → sink_->cancelled(), then loops back
  // to wait for the next user input.
  //
  // If no request is in flight, get_current_response_ch() returns null
  // and the signal_watcher_task does nothing — ESC is a harmless no-op.
  // -----------------------------------------------------------------------
  app.register_key(27, [&]() {
    if (input->slash_mode_active()) {
      input->cancel_slash_mode();
      return;
    }
    char cancel_byte = '1';
    ::write(g_sigint_pipe_write_fd, &cancel_byte, 1);
  });

  // -----------------------------------------------------------------------
  // Timer: detect approval mode changes and update the UI accordingly.
  // The prompt_bridge_task sets approval_state via App::post callbacks.
  // This timer polls the atomic and updates the divider/hint widgets.
  // -----------------------------------------------------------------------
  app.add_timer(50, [&]() {
    int state = approval_state.load(std::memory_order_acquire);
    if (state == 1) {
      // single_key mode — ensure UI reflects it.
      if (input->approval_mode_ != ChatInput::approval_mode::single_key) {
        input->set_approval_mode(ChatInput::approval_mode::single_key);
        input_divider->visible = true;
        input_divider->override_fg = Theme::current().warning;
        input_divider_top->override_fg = Theme::current().warning;
        input_hint->visible = true;
        input_hint->text =
          "  approval \xc2\xb7 y=approve n=reject r=reject+msg a=all ?=help";
        input_hint->hint_fg = Theme::current().warning;

        // The hint and divider becoming visible shrinks the scroll
        // area by 2 lines. flush_content (which ran in the posted
        // callback before this timer) set scroll_offset based on the
        // old, larger scroll height — so the last 2 lines of the
        // approval prompt are now hidden. Force a layout pass to
        // update scroll dimensions, then re-scroll to bottom if the
        // user was at the bottom.
        if (content_at_bottom) {
          if (auto cont = std::dynamic_pointer_cast<Container>(root)) {
            cont->layout();
          }
          scroll->scroll_offset = max_scroll_offset();
        }
      }
    } else if (state == 0) {
      // Normal mode — ensure UI is reset.
      if (input->approval_mode_ != ChatInput::approval_mode::none) {
        exit_approval_mode();
      }
    }
    // state == 2 (text_input) is set directly by on_approval_key, no
    // need to handle it here.
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
    sched.spawn<prompt_bridge_task>(
      prompt_ch, std::ref(agent_ready), std::ref(approval_state),
      [&app](std::function<void()> cb) { app.post(std::move(cb)); },
      append_content
    );

    auto sink = std::make_shared<mistral::agent::tui_render_sink>(
      [&app](std::function<void()> cb) { app.post(std::move(cb)); },
      append_fragment,
      flush_content
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

  // Restore SIGINT to default before closing the pipe write end,
  // so the signal handler doesn't write to a closed fd.
  signal(SIGINT, SIG_DFL);
  g_sigint_pipe_write_fd = -1;

  ::close(input_pipe_fds[1]);

  // Close the sigint pipe write end so the signal_watcher_task sees
  // EOF and exits.  This must happen before joining — otherwise the
  // signal_watcher blocks forever waiting for '0' from signal_shutdown()
  // which is only called from the agent's teardown, which may never run
  // if the agent is stuck in an in-flight API request.
  ::close(sigint_pipe_fds[1]);

  // Wait for dripcore thread to finish.
  if (dripcore_thread.joinable()) {
    dripcore_thread.join();
  }

  // Close remaining pipe read ends.
  ::close(input_pipe_fds[0]);
  ::close(sigint_pipe_fds[0]);

  return 0;
}