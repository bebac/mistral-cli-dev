#include "console/console_task.h"
#include "slash_command/slash_command.h"
#include "slash_command/slash_command_registry.h"

#include <isocline.h>

#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/select.h>
#include <cerrno>
#include <cstring>
#include <arpa/inet.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace mistral::console
{
  // Internal state shared between the dripcore coroutine and the isocline thread.
  struct console_state
  {
    // Notification values for pipe communication
    static constexpr char notify_input_ready_id = 1;
    static constexpr char notify_slash_cmd_pending_id = 2;
    static constexpr char notify_eof_id = 0;

    // Prompt signal values
    static constexpr char prompt_go_id = 1;
    static constexpr char prompt_shutdown_id = 0;

    // Pipe: thread writes to notify_write, coroutine reads from notify_read.
    int notify_read  = -1;
    int notify_write = -1;

    // Pipe: coroutine writes to prompt_write, thread reads from prompt_read.
    int prompt_read  = -1;
    int prompt_write = -1;

    // Thread-safe queue for input strings.
    mutable std::mutex mtx;
    std::queue<std::string> pending;
    std::atomic<bool> eof{false};
    std::atomic<bool> shutdown{false};

    std::thread thread;

    // Slash command registry (shared with the isocline thread for completion/validation)
    std::shared_ptr<slash_command::slash_command_registry> slash_cmd_registry;

    // For passing the prompt string to the input thread
    std::string current_prompt;
    bool is_approval_prompt = false;
    bool is_ask_prompt = false;

    // For passing slash commands from input thread to coroutine
    std::optional<slash_command::slash_command_request> pending_slash_command;

    // Thread-safe accessors
    void enqueue_input(std::string input)
    {
      std::lock_guard<std::mutex> lock(mtx);
      pending.push(std::move(input));
    }

    std::optional<std::string> dequeue_input()
    {
      std::lock_guard<std::mutex> lock(mtx);
      if (pending.empty())
        return std::nullopt;
      auto front = std::move(pending.front());
      pending.pop();
      return front;
    }

    void set_prompt(std::string prompt, bool approval, bool ask)
    {
      std::lock_guard<std::mutex> lock(mtx);
      current_prompt = std::move(prompt);
      is_approval_prompt = approval;
      is_ask_prompt = ask;
    }

    std::tuple<std::string, bool, bool> get_prompt() const
    {
      std::lock_guard<std::mutex> lock(mtx);
      return {current_prompt, is_approval_prompt, is_ask_prompt};
    }

    void set_pending_slash_command(slash_command::slash_command_request req)
    {
      std::lock_guard<std::mutex> lock(mtx);
      pending_slash_command.emplace(std::move(req));
    }

    std::optional<slash_command::slash_command_request> take_pending_slash_command()
    {
      std::lock_guard<std::mutex> lock(mtx);
      if (!pending_slash_command)
        return std::nullopt;
      auto req = std::move(*pending_slash_command);
      pending_slash_command.reset();
      return req;
    }

    // Helper methods for pipe notifications
    void notify_input_ready()
    {
      char n = notify_input_ready_id;
      ::write(notify_write, &n, 1);
    }

    void notify_slash_cmd_pending()
    {
      char n = notify_slash_cmd_pending_id;
      ::write(notify_write, &n, 1);
    }

    void notify_eof()
    {
      char n = notify_eof_id;
      ::write(notify_write, &n, 1);
    }

    void signal_prompt_go()
    {
      char s = prompt_go_id;
      ::write(prompt_write, &s, 1);
    }

    void signal_prompt_shutdown()
    {
      char s = prompt_shutdown_id;
      ::write(prompt_write, &s, 1);
    }
  };

  // Custom completer that handles slash commands
  static void slash_command_completer(ic_completion_env_t* cenv, const char* prefix)
  {
    auto* state = static_cast<console_state*>(ic_completion_arg(cenv));
    // Check if we're completing a slash command
    if (ic_starts_with(prefix, "/")) {
      // Get the part after the /
      const char* cmd_prefix = prefix + 1;
      if (state && state->slash_cmd_registry) {
        ic_complete_word(cenv, cmd_prefix,
          [](ic_completion_env_t* cenv, const char* prefix) {
            auto* s = static_cast<console_state*>(ic_completion_arg(cenv));
            if (s && s->slash_cmd_registry) {
              auto commands = s->slash_cmd_registry->get_command_names();
              std::vector<const char*> cmd_list;
              for (const auto& cmd : commands) {
                cmd_list.push_back(cmd.c_str());
              }
              cmd_list.push_back(nullptr);
              ic_add_completions(cenv, prefix, cmd_list.data());
            }
          },
          &ic_char_is_nonseparator);
      }
    }
    // For non-slash input, fall back to default filename completion
    else {
      ic_complete_filename(cenv, prefix, '/', nullptr, nullptr);
    }
  }

  // Handle execution of slash commands
  static void handle_slash_command(
    const std::string& input,
    console_state* state
  )
  {
    std::string cmd_input = input.substr(1);  // Remove leading '/'

    // Tokenize: find first space
    size_t space_pos = cmd_input.find(' ');
    std::string command = (space_pos == std::string::npos)
        ? cmd_input
        : cmd_input.substr(0, space_pos);
    std::string args = (space_pos == std::string::npos)
        ? ""
        : cmd_input.substr(space_pos + 1);

    if (!state->slash_cmd_registry) return;
    auto cmd = state->slash_cmd_registry->get_command(command);
    if (!cmd)
    {
      std::cout << "Unknown command: /" << command << std::endl;
      std::cout << "Try /help for available commands" << std::endl;
      ic_history_add(input.c_str());
      // Notify coroutine to show prompt again
      state->notify_input_ready();
      return;
    }

    // Add to history
    ic_history_add(input.c_str());

    // Queue the request for the coroutine to forward to the executor task
    state->set_pending_slash_command({command, args});
    // Signal the coroutine with a special value for slash commands
    state->notify_slash_cmd_pending();
  }

  // Reads a single character in raw mode for approval prompts
  // Returns empty string on EOF or error (after sending notification)
  static std::string read_approval_input(const std::string& prompt, console_state* state)
  {
    std::cout << prompt << std::flush;

    // Save current terminal attributes
    struct termios oldtio, newtio;
    tcgetattr(STDIN_FILENO, &oldtio);
    newtio = oldtio;
    newtio.c_lflag &= ~ICANON;  // Disable canonical mode (keep ECHO so user sees input)
    newtio.c_cc[VMIN] = 1;
    newtio.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &newtio);

    int ch = getchar();

    // Restore terminal attributes
    tcsetattr(STDIN_FILENO, TCSANOW, &oldtio);

    if (ch == EOF) {
      state->eof.store(true);
      state->notify_eof();
      return "";
    }
    // Terminal ECHO is enabled, so the character is already displayed
    return std::string(1, static_cast<char>(ch));
  }

  static void input_thread_func(console_state* state)
  {
    // Set up isocline history.
    try
    {
      namespace fs = std::filesystem;
      fs::path dir = fs::current_path() / ".mistral-cli";
      fs::create_directories(dir);
      std::string history_path = (dir / "history").string();
      ic_set_history(history_path.c_str(), -1);
    }
    catch (...)
    {
      ic_set_history(nullptr, -1);
    }

    // Set custom completer for slash commands
    ic_set_default_completer(&slash_command_completer, state);

    while (!state->shutdown.load())
    {
      // Get prompt read fd.
      int prompt_read_fd;
      {
        std::lock_guard<std::mutex> lock(state->mtx);
        prompt_read_fd = state->prompt_read;
      }

      // Wait for prompt signal from the coroutine
      char sig;
      ssize_t n = ::read(prompt_read_fd, &sig, 1);
      if (n <= 0 || state->shutdown.load()) break;

      std::string input;
      auto [prompt_to_display, approval_mode, ask_mode] = state->get_prompt();

      if (approval_mode)
      {
        input = read_approval_input(prompt_to_display, state);
        if (input.empty()) {
          // EOF or error already handled by read_approval_input
          continue;
        }
      }
      else
      {
        // Ask mode (ask_user tool): print the question block, then read
        // a full answer line with a short prompt.
        const char* readline_prompt = prompt_to_display.c_str();
        if (ask_mode) {
          std::cout << prompt_to_display << std::flush;
          readline_prompt = "Your answer: ";
        }
        // Chat mode: read full line
        char* raw = ic_readline(readline_prompt);
        if (!raw) {
          // EOF (Ctrl-D).
          state->eof.store(true);
          state->notify_eof();
          break;
        }

        input = std::string(raw);
        std::free(raw);

        // Check for slash command
        if (!input.empty() && input[0] == '/') {
          handle_slash_command(input, state);
          continue;
        }

        if (!input.empty()) {
          ic_history_add(input.c_str());
        }
      }

      state->enqueue_input(std::move(input));

      // Notify the event loop.
      state->notify_input_ready();
    }
  }

  console_task::console_task(
    std::shared_ptr<dripcore::channel<std::string>> prompt_ch,
    std::shared_ptr<dripcore::channel<std::string>> input_ch,
    std::shared_ptr<dripcore::channel<slash_command::slash_command_request>> slash_cmd_request_ch,
    std::shared_ptr<dripcore::channel<std::string>> slash_cmd_response_ch,
    std::shared_ptr<slash_command::slash_command_registry> slash_cmd_registry)
    :
    prompt_ch_(std::move(prompt_ch)),
    input_ch_(std::move(input_ch)),
    slash_cmd_request_ch_(std::move(slash_cmd_request_ch)),
    slash_cmd_response_ch_(std::move(slash_cmd_response_ch)),
    slash_cmd_registry_(std::move(slash_cmd_registry))
  {
  }

  console_task::~console_task() = default;

  dripcore::awaitable<> console_task::body()
  {
    state_ = std::make_shared<console_state>();

    // Create pipes.
    int notify_fds[2], prompt_fds[2];
    if (::pipe(notify_fds) != 0 || ::pipe(prompt_fds) != 0) {
      input_ch_->close();
      co_return;
    }

    state_->notify_read  = notify_fds[0];
    state_->notify_write = notify_fds[1];
    state_->prompt_read  = prompt_fds[0];
    state_->prompt_write = prompt_fds[1];

    // Make the notify read end non-blocking for dripcore.
    fcntl(state_->notify_read, F_SETFL,
          fcntl(state_->notify_read, F_GETFL) | O_NONBLOCK);

    // Pass the registry to the state for the input thread
    state_->slash_cmd_registry = slash_cmd_registry_;

    // Start the input thread.
    state_->thread = std::thread([this]() { input_thread_func(state_.get()); });

    // Main loop: wait for prompt requests, relay input.
    while (true)
    {
      auto prompt = co_await prompt_ch_->recv(this);
      if (!prompt) break;  // Agent closed the prompt channel.

      // Check for approval/ask markers
      const std::string approval_prefix = "[APPROVAL_PROMPT]";
      const std::string ask_prefix = "[ASK_USER]";
      bool is_approval = false;
      bool is_ask = false;
      std::string display_prompt;

      if (prompt->find(approval_prefix) == 0) {
        is_approval = true;
        display_prompt = prompt->substr(approval_prefix.length());
      }
      else if (prompt->find(ask_prefix) == 0) {
        is_ask = true;
        display_prompt = prompt->substr(ask_prefix.length());
      }
      else {
        display_prompt = *prompt;
      }

      // Store the prompt and mode in the state for the input thread
      state_->set_prompt(display_prompt, is_approval, is_ask);

      // Signal the thread to call readline (or readchar for approvals).
      state_->signal_prompt_go();

      // Wait for the thread to provide input.
      co_await readable(state_->notify_read);

      // Drain the notification byte.
      char buf;
      ::read(state_->notify_read, &buf, 1);

      if (state_->eof.load()) {
        break;
      }

      // Check if this is a slash command to forward
      if (buf == console_state::notify_slash_cmd_pending_id)
      {
        // Slash command: forward to executor task
        auto request = state_->take_pending_slash_command();
        if (request && slash_cmd_request_ch_) {
          slash_cmd_request_ch_->try_send(std::move(*request));
        }

        // Wait for response from executor task
        if (slash_cmd_response_ch_) {
          auto response = co_await slash_cmd_response_ch_->recv(this);
          if (response) {
            std::cout << *response << std::endl;
          }
        }

        // Slash command has completed. Send prompt to the prompt channel so that we immediately wake up to
        // redisplay the prompt again.
        prompt_ch_->try_send(display_prompt);
      }
      else
      {
        auto input_opt = state_->dequeue_input();
        std::string input = input_opt ? std::move(*input_opt) : "";
        input_ch_->try_send(std::move(input));
      }
    }

    co_return;
  }

  dripcore::awaitable<> console_task::teardown()
  {
    if ( input_ch_ ) {
      input_ch_->close();
    }

    if ( slash_cmd_request_ch_ ) {
      slash_cmd_request_ch_->close();
    }

    if ( slash_cmd_response_ch_ ) {
      slash_cmd_response_ch_->close();
    }

    if (state_) {
      state_->shutdown.store(true);

      // Wake the thread if it's blocked on prompt_read.
      state_->signal_prompt_shutdown();

      if (state_->thread.joinable()) {
        state_->thread.join();
      }

      {
        std::lock_guard<std::mutex> lock(state_->mtx);
        ::close(state_->notify_read);
        ::close(state_->notify_write);
        ::close(state_->prompt_read);
        ::close(state_->prompt_write);
      }
    }

    co_return;
  }
} // namespace mistral::console