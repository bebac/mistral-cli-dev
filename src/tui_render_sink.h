#ifndef MISTRAL_TUI_RENDER_SINK_H
#define MISTRAL_TUI_RENDER_SINK_H

#include "agent/render_sink.h"

#include <functional>
#include <string>

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
  // fragment to the content widget with auto-scroll handling.
  //
  // This header is TUI-specific (lives in src/). It depends only on the
  // abstract render_sink interface and std::function — no direct
  // dependency on cpptui.
  class tui_render_sink : public render_sink
  {
  public:
    using post_fn = std::function<void(std::function<void()>)>;
    using append_fn = std::function<void(const std::string&, content_type)>;

    tui_render_sink(post_fn post, append_fn append)
      : post_(std::move(post))
      , append_(std::move(append))
    {
    }

    void output(const std::string& text) override
    {
      post_([fn = append_, msg = text]() { fn(msg, content_type::normal); });
    }

    void output_thinking(const std::string& text) override
    {
      post_([fn = append_, msg = text]() { fn(msg, content_type::thinking); });
    }

    void output_line(const std::string& text) override
    {
      post_([fn = append_, msg = text + "\n"]() { fn(msg, content_type::tool_result); });
    }

    void output_tool_call(const std::string& name,
                          const std::string& args) override
    {
      post_([fn = append_, msg = "\n[tool] " + name + "(" + args + ")"]() {
        fn(msg, content_type::tool_call);
      });
    }

    void output_tool_result(const std::string& result) override
    {
      std::string display = (result.size() > 200)
        ? result.substr(0, 200) + "..."
        : result;
      post_([fn = append_, msg = " \xe2\x86\x92 " + display + "\n"]() {
        fn(msg, content_type::tool_result);
      });
    }

    void output_error(const std::string& text) override
    {
      post_([fn = append_, msg = "\n[error] " + text + "\n"]() { fn(msg, content_type::error); });
    }

    void output_warning(const std::string& text) override
    {
      post_([fn = append_, msg = "\n[warning] " + text + "\n"]() { fn(msg, content_type::warning); });
    }

    void thinking_begin() override
    {
      post_([fn = append_]() { fn("\n", content_type::normal); });
    }

    void thinking_end() override
    {
      post_([fn = append_]() { fn("\n\n", content_type::normal); });
    }

    void completed() override
    {
      post_([fn = append_]() { fn("\n", content_type::normal); });
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