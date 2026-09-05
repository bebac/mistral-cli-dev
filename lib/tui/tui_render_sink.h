#ifndef MISTRAL_TUI_RENDER_SINK_H
#define MISTRAL_TUI_RENDER_SINK_H

#include "agent/render_sink.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mistral::agent
{
  // Content type tag for styled rendering. The append_fn uses this to
  // tell the TUI which color/style to apply to the fragment.
  enum class content_type {
    normal,       // Regular AI response text
    thinking,      // AI thinking/reasoning
    tool_call,     // Tool call header (e.g. "[tool] write(...)")
    tool_result,   // Tool result or status (e.g. "→ result")
    error,         // Error messages
    warning,       // Warning messages
    user,          // User input echo
    system,        // System messages (approval prompts, etc.)
  };

  // TUI render sink: routes agent output to the TUI content widget via
  // App::post(). The post_fn schedules a callback on the UI thread
  // (typically cpptui::App::post). The append_fn appends a text
  // fragment to the content widget's styled text accumulator (O(1)).
  // The flush_fn performs the expensive O(n) re-wrap + scroll update
  // once after all batched fragments have been appended.
  //
  // Batching: instead of posting a callback per token (which floods the
  // UI event loop and starves input processing), fragments are accumulated
  // in a thread-safe buffer. A single callback is posted that drains all
  // accumulated fragments, calls append_fn for each, then calls flush_fn
  // once. This reduces the number of expensive wrap_text_static calls
  // from one-per-token to one-per-batch.
  //
  // This header is TUI-specific (lives in src/). It depends only on the
  // abstract render_sink interface and std::function — no direct
  // dependency on cpptui.
  class tui_render_sink : public render_sink
  {
  public:
    using post_fn = std::function<void(std::function<void()>)>;
    using append_fn = std::function<void(const std::string&, content_type)>;
    using flush_fn = std::function<void()>;

    tui_render_sink(post_fn post, append_fn append, flush_fn flush)
      : state_(std::make_shared<shared_state>())
    {
      state_->post = std::move(post);
      state_->append = std::move(append);
      state_->flush = std::move(flush);
    }

    void output(const std::string& text) override
    {
      state_->enqueue(text, content_type::normal);
    }

    void output_thinking(const std::string& text) override
    {
      state_->enqueue(text, content_type::thinking);
    }

    void output_line(const std::string& text) override
    {
      state_->enqueue(text + "\n", content_type::tool_result);
    }

    void output_tool_call(const std::string& name,
                          const std::string& args) override
    {
      state_->enqueue("\n[tool] " + name + "(" + args + ")",
                      content_type::tool_call);
    }

    void output_tool_result(const std::string& result) override
    {
      std::string display = (result.size() > 200)
        ? result.substr(0, 200) + "..."
        : result;
      state_->enqueue(" \xe2\x86\x92 " + display + "\n",
                      content_type::tool_result);
    }

    void output_error(const std::string& text) override
    {
      state_->enqueue("\n[error] " + text + "\n", content_type::error);
    }

    void output_warning(const std::string& text) override
    {
      state_->enqueue("\n[warning] " + text + "\n", content_type::warning);
    }

    void thinking_begin() override
    {
      state_->enqueue("\n", content_type::normal);
    }

    void thinking_end() override
    {
      state_->enqueue("\n\n", content_type::normal);
    }

    void completed() override
    {
      state_->enqueue("\n", content_type::normal);
    }

    void cancelled() override
    {
      state_->enqueue("\n[cancelled]\n", content_type::warning);
    }

  private:
    struct fragment {
      std::string text;
      content_type type;
    };

    // Shared state: captured by posted callbacks via shared_ptr so it
    // stays alive as long as any callback is pending, even if the sink
    // itself is destroyed.
    struct shared_state : std::enable_shared_from_this<shared_state> {
      post_fn   post;
      append_fn append;
      flush_fn  flush;
      std::mutex mutex;
      std::vector<fragment> buffer;
      std::atomic<bool> pending{false};

      // Called from the agent (dripcore) thread for every token.
      // Accumulates the fragment and ensures at most one drain callback
      // is posted at a time.
      void enqueue(std::string text, content_type type)
      {
        {
          std::lock_guard<std::mutex> lock(mutex);
          buffer.push_back({std::move(text), type});
        }
        bool expected = false;
        if (pending.compare_exchange_strong(expected, true)) {
          post([self = shared_from_this()]() {
            self->drain();
          });
        }
      }

      // Runs on the UI thread. Drains all accumulated fragments, calls
      // append_fn for each (O(1) per fragment), then calls flush_fn
      // once (O(n) re-wrap). If more fragments arrived during processing,
      // re-posts instead of clearing pending.
      void drain()
      {
        std::vector<fragment> drained;
        {
          std::lock_guard<std::mutex> lock(mutex);
          drained.swap(buffer);
        }
        for (auto& f : drained) {
          append(f.text, f.type);
        }
        if (flush) flush();

        // Check if more items arrived while we were processing.
        // Clear pending under the mutex to avoid a race where
        // enqueue() adds to the buffer between our empty check
        // and clearing pending — which would leave the item
        // stranded with no drain callback posted.
        bool should_repost = false;
        {
          std::lock_guard<std::mutex> lock(mutex);
          should_repost = !buffer.empty();
          if (!should_repost) {
            pending.store(false, std::memory_order_release);
          }
        }
        if (should_repost) {
          post([self = shared_from_this()]() {
            self->drain();
          });
        }
      }
    };

    std::shared_ptr<shared_state> state_;
  };
} // namespace mistral::agent

#endif // MISTRAL_TUI_RENDER_SINK_H