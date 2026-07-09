#ifndef MISTRAL_CONSOLE_TASK_H
#define MISTRAL_CONSOLE_TASK_H

#include <dripcore/task.h>
#include <dripcore/channel.h>
#include <dripcore/task_handle.h>

#include <memory>
#include <string>

namespace mistral::console
{
  // Console task integrates isocline (blocking readline) with dripcore's event loop.
  //
  // Architecture:
  //   - A background std::thread runs isocline readline calls.
  //   - A pipe pair provides notification from thread → event loop.
  //   - A second pipe (prompt_pipe) signals thread → "go ahead and call readline".
  //   - A mutex-protected queue transfers the input string.
  //
  // Flow:
  //   1. agent_task sends prompt string to prompt_ch
  //   2. console_task receives it, writes to prompt_pipe → wakes thread
  //   3. Thread calls ic_readline, stores result, writes to notify_pipe
  //   4. console_task co_await readable(notify_pipe), dequeues string
  //   5. console_task sends string to input_ch → agent_task receives it
  class console_task : public dripcore::task
  {
  public:
    console_task(
      std::shared_ptr<dripcore::channel<std::string>> prompt_ch,
      std::shared_ptr<dripcore::channel<std::string>> input_ch
    );

    ~console_task();

    dripcore::task_handle execute();

  private:
    std::shared_ptr<dripcore::channel<std::string>> prompt_ch_;
    std::shared_ptr<dripcore::channel<std::string>> input_ch_;
  };

} // namespace mistral::console

#endif // MISTRAL_CONSOLE_TASK_H
