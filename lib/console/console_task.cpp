#include "console/console_task.h"

#include <isocline.h>

#include <unistd.h>
#include <fcntl.h>
#include <termios.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace mistral::console
{
  // Internal state shared between the dripcore coroutine and the isocline thread.
  struct console_state
  {
    // Pipe: thread writes to notify_write, coroutine reads from notify_read.
    int notify_read  = -1;
    int notify_write = -1;

    // Pipe: coroutine writes to prompt_write, thread reads from prompt_read.
    int prompt_read  = -1;
    int prompt_write = -1;

    // Thread-safe queue for input strings.
    std::mutex mtx;
    std::queue<std::string> pending;
    std::atomic<bool> eof{false};
    std::atomic<bool> shutdown{false};

    std::thread thread;

    // For passing the prompt string to the input thread
    std::string current_prompt;
    bool is_approval_prompt = false;
  };

  static void input_thread_func(console_state* state)
  {
    // Set up isocline history.
    try {
      namespace fs = std::filesystem;
      fs::path dir = fs::current_path() / ".mistral-cli";
      fs::create_directories(dir);
      std::string history_path = (dir / "history").string();
      ic_set_history(history_path.c_str(), -1);
    } catch (...) {
      ic_set_history(nullptr, -1);
    }

    while (!state->shutdown.load())
    {
      // Wait for prompt signal from the coroutine.
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

      if (approval_mode) {
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
      } else {
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
    std::shared_ptr<dripcore::channel<std::string>> input_ch
  )
    : prompt_ch_(std::move(prompt_ch))
    , input_ch_(std::move(input_ch))
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

    // Make the notify read end non-blocking for dripcore.
    fcntl(state->notify_read, F_SETFL,
          fcntl(state->notify_read, F_GETFL) | O_NONBLOCK);

    // Start the input thread.
    state->thread = std::thread(input_thread_func, state.get());

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
      } else {
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

    // Cleanup.
    input_ch_->close();

    state->shutdown.store(true);

    // Wake the thread if it's blocked on prompt_read.
    char sig = 0;
    ::write(state->prompt_write, &sig, 1);

    if (state->thread.joinable()) {
      state->thread.detach();  // Don't block the event loop waiting for thread join.
    }

    ::close(state->notify_read);
    ::close(state->notify_write);
    ::close(state->prompt_read);
    ::close(state->prompt_write);

    co_return;
  }

} // namespace mistral::console
