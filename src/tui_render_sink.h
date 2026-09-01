#ifndef MISTRAL_TUI_RENDER_SINK_H
#define MISTRAL_TUI_RENDER_SINK_H

#include "agent/render_sink.h"

#include <functional>
#include <string>

namespace mistral::agent
{
  // TUI render sink: routes agent output to the TUI content widget via
  // App::post(). The post_fn schedules a callback on the UI thread
  // (typically cpptui::App::post). The append_fn appends a text
  // fragment to the content widget with auto-scroll handling.
  //
  // This header is TUI-specific (lives in src/). It depends only on the
  // abstract render_sink interface and std::function — no direct
  // dependency on cpptui.
  class tui_render_sink : public render_sink
  {
  public:
    using post_fn = std::function<void(std::function<void()>)>;
    using append_fn = std::function<void(const std::string&)>;

    tui_render_sink(post_fn post, append_fn append)
      : post_(std::move(post))
      , append_(std::move(append))
    {
    }

    void output(const std::string& text) override
    {
      post_([fn = append_, msg = text]() { fn(msg); });
    }

    void output_thinking(const std::string& text) override
    {
      post_([fn = append_, msg = text]() { fn(msg); });
    }

    void output_line(const std::string& text) override
    {
      post_([fn = append_, msg = text + "\n"]() { fn(msg); });
    }

    void output_tool_call(const std::string& name,
                          const std::string& args) override
    {
      post_([fn = append_, msg = "\n[tool] " + name + "(" + args + ")"]() {
        fn(msg);
      });
    }

    void output_tool_result(const std::string& result) override
    {
      std::string display = (result.size() > 200)
        ? result.substr(0, 200) + "..."
        : result;
      post_([fn = append_, msg = " \xe2\x86\x92 " + display + "\n"]() {
        fn(msg);
      });
    }

    void output_error(const std::string& text) override
    {
      post_([fn = append_, msg = "\n[error] " + text + "\n"]() { fn(msg); });
    }

    void output_warning(const std::string& text) override
    {
      post_([fn = append_, msg = "\n[warning] " + text + "\n"]() { fn(msg); });
    }

    void thinking_begin() override
    {
      post_([fn = append_]() { fn("\n"); });
    }

    void thinking_end() override
    {
      post_([fn = append_]() { fn("\n\n"); });
    }

    void completed() override
    {
      post_([fn = append_]() { fn("\n"); });
    }

    void cancelled() override
    {
    }

  private:
    post_fn post_;
    append_fn append_;
  };
} // namespace mistral::agent

#endif // MISTRAL_TUI_RENDER_SINK_H