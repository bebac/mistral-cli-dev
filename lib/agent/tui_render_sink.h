#ifndef MISTRAL_TUI_RENDER_SINK_H
#define MISTRAL_TUI_RENDER_SINK_H

#include "agent/render_sink.h"

#include <atomic>
#include <mutex>
#include <string>

namespace mistral::agent
{
  struct tui_output_queue
  {
    std::mutex mtx;
    std::string pending;
    std::atomic<bool> has_data{false};

    void push(const std::string& fragment)
    {
      {
        std::lock_guard<std::mutex> lock(mtx);
        pending += fragment;
      }
      has_data.store(true, std::memory_order_release);
    }

    std::string drain()
    {
      std::lock_guard<std::mutex> lock(mtx);
      std::string result;
      result.swap(pending);
      has_data.store(false, std::memory_order_release);
      return result;
    }
  };

  class tui_render_sink : public render_sink
  {
  public:
    explicit tui_render_sink(tui_output_queue& queue)
      : queue_(queue)
    {
    }

    void output(const std::string& text) override
    {
      queue_.push(text);
    }

    void output_thinking(const std::string& text) override
    {
      queue_.push(text);
    }

    void output_line(const std::string& text) override
    {
      queue_.push(text + "\n");
    }

    void output_tool_call(const std::string& name,
                          const std::string& args) override
    {
      queue_.push("\n[tool] " + name + "(" + args + ")");
    }

    void output_tool_result(const std::string& result) override
    {
      std::string display = (result.size() > 200)
        ? result.substr(0, 200) + "..."
        : result;
      queue_.push(" \xe2\x86\x92 " + display + "\n");
    }

    void output_error(const std::string& text) override
    {
      queue_.push("\n[error] " + text + "\n");
    }

    void output_warning(const std::string& text) override
    {
      queue_.push("\n[warning] " + text + "\n");
    }

    void thinking_begin() override
    {
      queue_.push("\n");
    }

    void thinking_end() override
    {
      queue_.push("\n\n");
    }

    void completed() override
    {
      queue_.push("\n");
    }

    void cancelled() override
    {
    }

  private:
    tui_output_queue& queue_;
  };
} // namespace mistral::agent

#endif // MISTRAL_TUI_RENDER_SINK_H