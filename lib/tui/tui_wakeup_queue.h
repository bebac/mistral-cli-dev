#ifndef MISTRAL_TUI_WAKEUP_QUEUE_H
#define MISTRAL_TUI_WAKEUP_QUEUE_H

#include "common/wakeup_pipe.h"

#include <deque>
#include <mutex>
#include <utility>

namespace mistral::tui
{
  // Queue for the UI thread -> dripcore thread direction.
  //
  // The UI thread calls push(), which enqueues the item and wakes the
  // consumer through a self-pipe. The bridge task awaits the pipe, then
  // calls drain().
  //
  // Owning the queue and the wakeup together is the point: a queue that is
  // pushed without a wakeup stalls silently, and a wakeup without a push is
  // wasted work. Keeping them in one object makes that pairing impossible
  // to get wrong.
  template <typename T> class wakeup_queue
  {
  public:
    // Enqueue an item and wake the consumer. Safe to call from any thread.
    void push(T item)
    {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(std::move(item));
      }
      // Notify after releasing the lock: the consumer only ever takes the
      // lock to drain, so holding it across the write would be pointless.
      //
      // The result is not checked: notify() already retries on EINTR, so the
      // only remaining failure is a closed reader, which means shutdown.
      pipe_.notify();
    }

    // Everything enqueued so far. Called by the consumer after a wakeup.
    std::deque<T> drain()
    {
      std::lock_guard<std::mutex> lock(mutex_);
      std::deque<T> result;
      result.swap(items_);
      return result;
    }

    // Read end of the wakeup pipe, for the consumer to await.
    int read_fd() const
    {
      return pipe_.read_fd();
    }

    // Close the write end so the consumer sees EOF and shuts down.
    void close_write()
    {
      pipe_.close_write();
    }

    void close_read()
    {
      pipe_.close_read();
    }

  private:
    std::mutex mutex_;
    std::deque<T> items_;
    common::wakeup_pipe pipe_;
  };
} // namespace mistral::tui

#endif // MISTRAL_TUI_WAKEUP_QUEUE_H
