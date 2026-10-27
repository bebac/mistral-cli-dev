#ifndef MISTRAL_TUI_THREAD_SAFE_QUEUE_H
#define MISTRAL_TUI_THREAD_SAFE_QUEUE_H

#include <deque>
#include <mutex>
#include <utility>

namespace mistral::tui
{
  // Thread-safe queue for cross-thread communication (TUI thread →
  // dripcore thread). The TUI thread calls push(); the dripcore bridge
  // task calls drain() after being woken up via a pipe.
  //
  // This replaces the former input_queue and slash_command_queue structs
  // which were identical except for the element type.
  template <typename T> class thread_safe_queue
  {
  public:
    void push(T item)
    {
      std::lock_guard<std::mutex> lock(mutex_);
      items_.push_back(std::move(item));
    }

    std::deque<T> drain()
    {
      std::lock_guard<std::mutex> lock(mutex_);
      std::deque<T> result;
      result.swap(items_);
      return result;
    }

  private:
    std::mutex mutex_;
    std::deque<T> items_;
  };
} // namespace mistral::tui

#endif // MISTRAL_TUI_THREAD_SAFE_QUEUE_H
