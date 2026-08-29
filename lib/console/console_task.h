#ifndef MISTRAL_CONSOLE_TASK_H
#define MISTRAL_CONSOLE_TASK_H

#include <console/slash_command.h>
#include <console/slash_command_registry.h>

#include <dripcore/task.h>
#include <dripcore/channel.h>
#include <dripcore/awaitable.h>

#include <memory>
#include <string>

namespace mistral::console
{
  // Forward declaration — defined in console_task.cpp
  struct console_state;

  // Console task integrates isocline (blocking readline) with dripcore's event loop.
  //
  // Architecture:
  //   - A background std::thread runs isocline readline calls.
  //   - A pipe pair provides notification from thread to event loop.
  //   - A second pipe (prompt_pipe) signals thread to call readline.
  //   - A mutex-protected queue transfers the input string.
  //
  // Flow:
  //   1. agent_task sends prompt string to prompt_ch
  //   2. console_task receives it, writes to prompt_pipe, wakes thread
  //   3. Thread calls ic_readline, stores result, writes to notify_pipe
  //   4. console_task co_await readable(notify_pipe), dequeues string
  //   5. console_task sends string to input_ch, agent_task receives it
  class console_task : public dripcore::task
  {
  public:
    console_task(
      std::shared_ptr<dripcore::channel<std::string>> prompt_ch,
      std::shared_ptr<dripcore::channel<std::string>> input_ch,
      std::shared_ptr<dripcore::channel<slash_command_request>> slash_cmd_request_ch = nullptr,
      std::shared_ptr<dripcore::channel<std::string>> slash_cmd_response_ch = nullptr,
      std::shared_ptr<slash_command_registry> slash_cmd_registry = nullptr
    );

    ~console_task();

  protected:
    dripcore::awaitable<> body() override;
    dripcore::awaitable<> teardown() override;

  private:
    std::shared_ptr<dripcore::channel<std::string>> prompt_ch_;
    std::shared_ptr<dripcore::channel<std::string>> input_ch_;
    std::shared_ptr<dripcore::channel<slash_command_request>> slash_cmd_request_ch_;
    std::shared_ptr<dripcore::channel<std::string>> slash_cmd_response_ch_;
    std::shared_ptr<slash_command_registry> slash_cmd_registry_;
    std::shared_ptr<console_state> state_;
  };

} // namespace mistral::console

#endif // MISTRAL_CONSOLE_TASK_H