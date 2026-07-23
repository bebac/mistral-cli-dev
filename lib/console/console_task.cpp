#include "console/console_task.h"
#include "console/slash_command.h"
#include "console/slash_command_registry.h"

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
#include <vector>

namespace mistral::console
{
  namespace
  {
    // Helper to read a length-prefixed string from a file descriptor
    static std::string read_length_prefixed(int fd)
    {
      uint32_t len;
      ssize_t n = ::read(fd, &len, sizeof(len));
      if (n != static_cast<ssize_t>(sizeof(len)))
      {
        return "";
      }
      len = ntohl(len);
      if (len == 0) {
        return "";
      }

      std::string response;
      response.resize(len);
      size_t bytes_read = 0;
      while (bytes_read < len)
      {
        ssize_t chunk = ::read(fd, response.data() + bytes_read, len - bytes_read);
        if (chunk <= 0) {
          break;
        }
        bytes_read += chunk;
      }
      return (bytes_read == len) ? response : "";
    }
  }

  // Internal state shared between the dripcore coroutine and the isocline thread.
  struct console_state
  {
    // Pipe: thread writes to notify_write, coroutine reads from notify_read.
    int notify_read  = -1;
    int notify_write = -1;

    // Pipe: coroutine writes to prompt_write, thread reads from prompt_read.
    int prompt_read  = -1;
    int prompt_write = -1;

    // Pipe: executor task fd for receiving slash command reponse.
    int slash_resp_read  = -1;
    //int slash_resp_write = -1;

    // Thread-safe queue for input strings.
    std::mutex mtx;
    std::queue<std::string> pending;
    std::atomic<bool> eof{false};
    std::atomic<bool> shutdown{false};

    std::thread thread;

    // For passing the prompt string to the input thread
    std::string current_prompt;
    bool is_approval_prompt = false;

    // For passing slash commands from input thread to coroutine
    std::optional<slash_command_request> pending_slash_command;
  };

  // Handle slash command completion
  static void handle_slash_completion(ic_completion_env_t* cenv, const char* prefix)
  {
    auto& registry = slash_command_registry::instance();
    auto commands = registry.get_command_names();

    // Convert to null-terminated array of const char* for ic_add_completions
    std::vector<const char*> cmd_list;
    for (const auto& cmd : commands) {
        cmd_list.push_back(cmd.c_str());
    }
    cmd_list.push_back(nullptr);

    ic_add_completions(cenv, prefix, cmd_list.data());
  }

  // Custom completer that handles slash commands
  static void slash_command_completer(ic_completion_env_t* cenv, const char* prefix)
  {
    // Check if we're completing a slash command
    if (ic_starts_with(prefix, "/")) {
      // Get the part after the /
      const char* cmd_prefix = prefix + 1;
      ic_complete_word(cenv, cmd_prefix, &handle_slash_completion, &ic_char_is_nonseparator);
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

    auto& registry = slash_command_registry::instance();
    auto cmd = registry.get_command(command);
    if (!cmd)
    {
      std::cout << "Unknown command: /" << command << std::endl;
      std::cout << "Try /help for available commands" << std::endl;
      ic_history_add(input.c_str());
      // Notify coroutine to show prompt again
      char notify = 1;
      ::write(state->notify_write, &notify, 1);
      return;
    }

    // Add to history
    ic_history_add(input.c_str());

    if (cmd->context() == slash_command::execution_context::IMMEDIATE)
    {
      std::string response = const_cast<slash_command *>(cmd)->execute_immediate(args);
      std::cout << response << std::endl;
      // Notify coroutine to show prompt again
      char notify = 1;
      ::write(state->notify_write, &notify, 1);
    }
    else
    {
      // AGENT context: queue the request for the coroutine to forward
      // We cannot call try_send() from this std::thread context
      {
        std::lock_guard<std::mutex> lock(state->mtx);
        state->pending_slash_command.emplace();
        state->pending_slash_command->command_name = command;
        state->pending_slash_command->args = args;
      }
      // Signal the coroutine with a special value for slash commands
      char notify = 2; // 2 = slash command pending
      ::write(state->notify_write, &notify, 1);
    }
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
    ic_set_default_completer(&slash_command_completer, nullptr);

    while (!state->shutdown.load())
    {
      // init local copies of pipe fds.
      int prompt_read_fd, slash_resp_read_fd;
      {
        std::lock_guard<std::mutex> lock(state->mtx);
        prompt_read_fd = state->prompt_read;
        slash_resp_read_fd = state->slash_resp_read;
      }

      // Wait for prompt signal from the coroutine OR slash response from executor task
      fd_set read_fds;
      FD_ZERO(&read_fds);
      if (prompt_read_fd != -1)
        FD_SET(prompt_read_fd, &read_fds);
      if (slash_resp_read_fd != -1)
        FD_SET(slash_resp_read_fd, &read_fds);
      int max_fd = std::max(prompt_read_fd, slash_resp_read_fd) + 1;
      int ret = select(max_fd, &read_fds, nullptr, nullptr, nullptr);

      // Handle select() return values
      if (ret < 0) {
        if (errno == EINTR) continue;  // Interrupted by signal, retry immediately
        if (errno == EBADF) break;
        std::cerr << "select() error in console input thread: " << strerror(errno) << std::endl;
        continue;
      }
      if (ret == 0) {
        continue;  // Timeout (shouldn't happen with nullptr timeout)
      }

      if (state->shutdown.load()) {
        break;
      }

      // Check if we have a slash command response to display
      if (slash_resp_read_fd != -1 && FD_ISSET(slash_resp_read_fd, &read_fds)) {
        std::string response = read_length_prefixed(slash_resp_read_fd);
        if (!response.empty()) {
            std::cout << response << std::endl;
        }
        // Notify coroutine to show prompt again
        char notify = 1;
        ::write(state->notify_write, &notify, 1);
        continue;
      }

      // Check for prompt signal (not slash response)
      if (!FD_ISSET(prompt_read_fd, &read_fds)) {
        continue;
      }

      // Read the prompt signal
      char sig;
      ssize_t n = ::read(state->prompt_read, &sig, 1);
      if (n <= 0 || state->shutdown.load()) break;

      std::string input;
      std::string prompt_to_display;
      bool approval_mode = false;

      {
        std::lock_guard<std::mutex> lock(state->mtx);
        prompt_to_display = state->current_prompt;
        approval_mode = state->is_approval_prompt;
      }

      if (approval_mode)
      {
        // Approval mode: read single character
        // Display the approval prompt first
        std::cout << prompt_to_display << std::flush;

        // We need to put the terminal in raw mode to read a single char without Enter
        // Save current terminal attributes
        struct termios oldtio, newtio;
        tcgetattr(STDIN_FILENO, &oldtio);
        newtio = oldtio;
        newtio.c_lflag &= ~ICANON;  // Disable canonical mode (but keep ECHO so user sees what they type)
        newtio.c_cc[VMIN] = 1;
        newtio.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &newtio);

        int ch = getchar();

        // Restore terminal attributes
        tcsetattr(STDIN_FILENO, TCSANOW, &oldtio);

        if (ch == EOF) {
          state->eof.store(true);
          char notify = 0;
          ::write(state->notify_write, &notify, 1);
          break;
        }
        // Note: Terminal ECHO is enabled, so the character is already displayed
        input = std::string(1, static_cast<char>(ch));
      }
      else
      {
        // Chat mode: read full line
        char* raw = ic_readline(prompt_to_display.c_str());
        if (!raw) {
          // EOF (Ctrl-D).
          state->eof.store(true);
          char notify = 0;
          ::write(state->notify_write, &notify, 1);
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

      {
        std::lock_guard<std::mutex> lock(state->mtx);
        state->pending.push(std::move(input));
      }

      // Notify the event loop.
      char notify = 1;
      ::write(state->notify_write, &notify, 1);
    }
  }

  console_task::console_task(
    std::shared_ptr<dripcore::channel<std::string>> prompt_ch,
    std::shared_ptr<dripcore::channel<std::string>> input_ch,
    std::shared_ptr<dripcore::channel<slash_command_request>> slash_cmd_request_ch,
    int slash_resp_read_fd
  )
    : prompt_ch_(std::move(prompt_ch))
    , input_ch_(std::move(input_ch))
    , slash_cmd_request_ch_(std::move(slash_cmd_request_ch))
    , slash_resp_read_fd_(slash_resp_read_fd)
  {}

  console_task::~console_task() = default;

  dripcore::task_handle console_task::execute()
  {
    auto state = std::make_shared<console_state>();

    // Create pipes.
    int notify_fds[2], prompt_fds[2];
    if (::pipe(notify_fds) != 0 || ::pipe(prompt_fds) != 0) {
      input_ch_->close();
      co_return;
    }

    state->notify_read  = notify_fds[0];
    state->notify_write = notify_fds[1];
    state->prompt_read  = prompt_fds[0];
    state->prompt_write = prompt_fds[1];
    state->slash_resp_read = slash_resp_read_fd_;

    // Make the notify read end non-blocking for dripcore.
    fcntl(state->notify_read, F_SETFL,
          fcntl(state->notify_read, F_GETFL) | O_NONBLOCK);

    // Start the input thread.
    state->thread = std::thread([state]() { input_thread_func(state.get()); });

    // Main loop: wait for prompt requests, relay input.
    while (true)
    {
      auto prompt = co_await prompt_ch_->recv();
      if (!prompt) break;  // Agent closed the prompt channel.

      // Check for approval marker
      const std::string approval_prefix = "[APPROVAL_PROMPT]";
      bool is_approval = false;
      std::string display_prompt;

      if (prompt->find(approval_prefix) == 0) {
        is_approval = true;
        display_prompt = prompt->substr(approval_prefix.length());
      }
      else {
        display_prompt = *prompt;
      }

      // Store the prompt and mode in the state for the input thread
      {
        std::lock_guard<std::mutex> lock(state->mtx);
        state->current_prompt = display_prompt;
        state->is_approval_prompt = is_approval;
      }

      // Signal the thread to call readline (or readchar for approvals).
      char sig = 1;  // Just a signal, the actual mode is stored in state
      ::write(state->prompt_write, &sig, 1);

      // Wait for the thread to provide input.
      co_await readable(state->notify_read);

      // Drain the notification byte.
      char buf;
      ::read(state->notify_read, &buf, 1);

      if (state->eof.load()) {
        break;
      }

      // Check if this is a slash command to forward
      if (buf == 2)
      {
        // Slash command: forward to executor task
        std::optional<slash_command_request> request;
        {
          std::lock_guard<std::mutex> lock(state->mtx);
          request = std::move(state->pending_slash_command);
          state->pending_slash_command.reset();
        }
        if (request && slash_cmd_request_ch_) {
          slash_cmd_request_ch_->try_send(std::move(*request));
        }

        // Wait for notification from input thread that response has been displayed
        co_await readable(state->notify_read);
        char notify_byte;
        ::read(state->notify_read, &notify_byte, 1);

        // Now go back to wait for next prompt from agent. It's a bit quirky that we rely on the
        // agent task to resubmit the prompt when it get an empty input (lib/agent/interactive_agent_task.h:50)
        input_ch_->try_send("");
      }
      else
      {
        std::string input;
        {
          std::lock_guard<std::mutex> lock(state->mtx);
          if (!state->pending.empty()) {
            input = std::move(state->pending.front());
            state->pending.pop();
          }
        }
        input_ch_->try_send(std::move(input));
      }
    }

    // Cleanup.
    if ( input_ch_ ) {
      input_ch_->close();
    }

    if ( slash_cmd_request_ch_ ) {
      slash_cmd_request_ch_->close();
    }

    state->shutdown.store(true);

    // Wake the thread if it's blocked on prompt_read.
    char sig = 0;
    ::write(state->prompt_write, &sig, 1);

    if (state->thread.joinable()) {
      state->thread.join();
    }

    {
      std::lock_guard<std::mutex> lock(state->mtx);
      ::close(state->notify_read);
      ::close(state->notify_write);
      ::close(state->prompt_read);
      ::close(state->prompt_write);
      if (state->slash_resp_read != -1) {
        ::close(state->slash_resp_read);
        state->slash_resp_read = -1;
      }
    }

    co_return;
  }

} // namespace mistral::console
