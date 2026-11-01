#ifndef MISTRAL_TUI_BRIDGE_TASKS_H
#define MISTRAL_TUI_BRIDGE_TASKS_H

#include "common/shutdown_signal.h"
#include "tui/tui_thread_safe_queue.h"
#include "tui/tui_types.h"

#include <dripcore/channel.h>
#include <dripcore/task.h>

#include <functional>
#include <memory>
#include <unistd.h>

namespace mistral::tui
{
  // ---------------------------------------------------------------------------
  // ui_bridge_task: generic dripcore task that forwards messages from a
  // dripcore channel to the UI thread.
  //
  // Each message is handed to post_fn, which schedules the apply callback on
  // the UI event loop. The callback owns the message by value, so it must not
  // capture `this` of a task: tasks die with the scheduler while callbacks may
  // still be queued.
  //
  // When a shutdown signal is supplied, the bridge registers a callback that
  // closes its own data channel, so its exit never depends on the producer
  // closing that channel. Registration happens in the constructor, which runs
  // on the scheduler thread before run(), so the bridge is covered even if it
  // is already suspended on recv() when shutdown fires.
  //
  // This replaces the former per-consumer bridge tasks (prompt_bridge_task,
  // slash_command_response_receiver), which differed only in how they
  // interpreted the message.
  // ---------------------------------------------------------------------------
  template <typename T> class ui_bridge_task : public dripcore::task
  {
  public:
    using apply_fn = std::function<void(T)>;

    ui_bridge_task(
      std::shared_ptr<dripcore::channel<T>> ch,
      post_fn post,
      apply_fn apply,
      std::shared_ptr<common::shutdown_signal> shutdown = nullptr
    )
      : ch_(std::move(ch)), post_(std::move(post)), apply_(std::move(apply)), shutdown_(std::move(shutdown))
    {
      if (shutdown_)
      {
        // Closing our data channel is what releases the recv() below.
        shutdown_->add([ch = ch_]() { ch->close(); });
      }
    }

    dripcore::awaitable<> body() override
    {
      while (true)
      {
        auto msg = co_await ch_->recv(this);
        if (!msg)
        {
          break; // Channel closed.
        }

        post_([apply = apply_, value = std::move(*msg)]() mutable { apply(std::move(value)); });
      }

      co_return;
    }

  private:
    std::shared_ptr<dripcore::channel<T>> ch_;
    post_fn post_;
    apply_fn apply_;
    std::shared_ptr<common::shutdown_signal> shutdown_;
  };

  // ---------------------------------------------------------------------------
  // pipe_bridge_task: generic dripcore task that bridges a thread-safe
  // queue (fed from the UI thread) to a dripcore channel (consumed in
  // the agent task). A self-pipe provides the cross-thread wakeup:
  //
  //   UI thread:  queue.push(item);  ::write(pipe_write_fd, "1", 1);
  //   dripcore:   co_await readable(pipe_read_fd);
  //               ::read(pipe_read_fd, ...);  // drain wakeup byte
  //               items = queue.drain();
  //               for (auto& item : items) target_ch->try_send(std::move(item));
  //
  // On pipe EOF (write end closed during shutdown), the target channel is
  // closed and the optional on_eof callback is invoked (e.g. to close
  // additional channels). The task then exits.
  //
  // This replaces the former input_bridge_task and slash_command_bridge_task
  // which were structurally identical except for the element type and the
  // set of channels to close on EOF.
  // ---------------------------------------------------------------------------
  template <typename T> class pipe_bridge_task : public dripcore::task
  {
  public:
    using cleanup_fn = std::function<void()>;

    pipe_bridge_task(
      int pipe_read_fd,
      std::shared_ptr<dripcore::channel<T>> target_ch,
      thread_safe_queue<T>& queue,
      cleanup_fn on_eof = nullptr
    )
      : pipe_read_fd_(pipe_read_fd), target_ch_(std::move(target_ch)), queue_(queue), on_eof_(std::move(on_eof))
    {
    }

    dripcore::awaitable<> body() override
    {
      while (true)
      {
        co_await readable(pipe_read_fd_);

        char buf[16];
        ssize_t n = ::read(pipe_read_fd_, buf, sizeof(buf));

        if (n <= 0)
        {
          // EOF — pipe write end closed, shutdown.
          target_ch_->close();
          if (on_eof_)
            on_eof_();
          co_return;
        }

        // Drain all pending items and send to target channel.
        auto items = queue_.drain();
        for (auto& item : items)
        {
          target_ch_->try_send(std::move(item));
        }
      }

      co_return;
    }

  private:
    int pipe_read_fd_;
    std::shared_ptr<dripcore::channel<T>> target_ch_;
    thread_safe_queue<T>& queue_;
    cleanup_fn on_eof_;
  };

} // namespace mistral::tui

#endif // MISTRAL_TUI_BRIDGE_TASKS_H
