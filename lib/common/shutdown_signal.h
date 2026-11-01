#ifndef MISTRAL_COMMON_SHUTDOWN_SIGNAL_H
#define MISTRAL_COMMON_SHUTDOWN_SIGNAL_H

#include <functional>
#include <utility>
#include <vector>

namespace mistral::common
{
  // Fan-out for "the application is shutting down".
  //
  // This is deliberately not a dripcore channel: a channel holds a single
  // waiting receiver, so several tasks awaiting one channel would leave all
  // but the last suspended forever. Instead each interested task registers a
  // callback, and fire() runs them all.
  //
  // A task that waits on a channel whose producer may never close it
  // registers a callback that closes that channel, so its exit does not
  // depend on remembering to close every data channel individually.
  //
  // Not thread-safe: register and fire on the scheduler thread.
  class shutdown_signal
  {
  public:
    // Register a callback to run on shutdown. Runs at most once: if the
    // signal has already fired, it runs immediately.
    void add(std::function<void()> on_shutdown)
    {
      if (fired_)
      {
        on_shutdown();
        return;
      }
      handlers_.push_back(std::move(on_shutdown));
    }

    // Run every registered callback and clear the list.
    void fire()
    {
      fired_ = true;
      auto handlers = std::move(handlers_);
      handlers_.clear();
      for (auto& handler : handlers)
      {
        handler();
      }
    }

    bool fired() const
    {
      return fired_;
    }

  private:
    std::vector<std::function<void()>> handlers_;
    bool fired_ = false;
  };
} // namespace mistral::common

#endif // MISTRAL_COMMON_SHUTDOWN_SIGNAL_H
