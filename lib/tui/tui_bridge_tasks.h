#ifndef MISTRAL_TUI_BRIDGE_TASKS_H
#define MISTRAL_TUI_BRIDGE_TASKS_H

#include "common/shutdown_signal.h"
#include "tui/tui_types.h"
#include "tui/tui_wakeup_queue.h"

#include <dripcore/channel.h>
#include <dripcore/task.h>

#include <cerrno>
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

  // What a read on a wakeup pipe means.
  enum class pipe_read_outcome
  {
    data,   ///< Bytes were read: drain the queue.
    eof,    ///< The write end is closed: shut down.
    retry,  ///< Interrupted by a signal, nothing consumed: read again.
    failed, ///< The read end is gone (EBADF) or unusable: shut down.
  };

  // Classify a ::read result on a wakeup pipe.
  //
  // A signal interrupts the read without consuming anything, so it must not
  // be mistaken for EOF: that would close the target channel and end the
  // agent task on a spurious wakeup. SIGINT is installed with sa_flags = 0
  // (no SA_RESTART), so EINTR is reachable in production.
  inline pipe_read_outcome classify_pipe_read(ssize_t n, int err)
  {
    if (n > 0)
    {
      return pipe_read_outcome::data;
    }
    if (n == 0)
    {
      return pipe_read_outcome::eof;
    }
    return err == EINTR ? pipe_read_outcome::retry : pipe_read_outcome::failed;
  }

  // ---------------------------------------------------------------------------
  // pipe_bridge_task: generic dripcore task that bridges a wakeup_queue
  // (fed from the UI thread) to a dripcore channel (consumed in the agent
  // task). The queue owns its self-pipe, which provides the cross-thread
  // wakeup:
  //
  //   UI thread:  queue.push(item);   // enqueues and writes the wakeup byte
  //   dripcore:   co_await readable(queue.read_fd());
  //               ::read(queue.read_fd(), ...);  // drain wakeup byte
  //               items = queue.drain();
  //               for (auto& item : items) target_ch->try_send(std::move(item));
  //
  // On pipe EOF (write end closed during shutdown), the target channel is
  // closed and the task exits. Channels that other bridges are waiting on
  // are released through common::shutdown_signal, not from here, so this
  // task needs no cleanup callback.
  //
  // This replaces the former input_bridge_task and slash_command_bridge_task
  // which were structurally identical except for the element type.
  // ---------------------------------------------------------------------------
  template <typename T> class pipe_bridge_task : public dripcore::task
  {
  public:
    pipe_bridge_task(wakeup_queue<T>& queue, std::shared_ptr<dripcore::channel<T>> target_ch)
      : queue_(queue), target_ch_(std::move(target_ch))
    {
    }

    dripcore::awaitable<> body() override
    {
      while (true)
      {
        co_await readable(queue_.read_fd());

        char buf[16];
        const ssize_t n = ::read(queue_.read_fd(), buf, sizeof(buf));
        const pipe_read_outcome outcome = classify_pipe_read(n, errno);

        if (outcome == pipe_read_outcome::retry)
        {
          // Nothing was consumed, so the wakeup byte is still in the pipe and
          // readable() fires again immediately.
          continue;
        }

        if (outcome != pipe_read_outcome::data)
        {
          // EOF (write end closed) or a dead read end: nothing more can
          // arrive.
          target_ch_->close();
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
    wakeup_queue<T>& queue_;
    std::shared_ptr<dripcore::channel<T>> target_ch_;
  };

} // namespace mistral::tui

#endif // MISTRAL_TUI_BRIDGE_TASKS_H
