#ifndef MISTRAL_RENDER_SINK_H
#define MISTRAL_RENDER_SINK_H

#include <string>

namespace mistral::agent
{
  class render_sink
  {
  public:
    virtual ~render_sink() = default;

    virtual void output(const std::string& text) = 0;
    virtual void output_thinking(const std::string& text) = 0;
    virtual void output_line(const std::string& text) = 0;
    // Standalone status line (e.g. compaction progress). Unlike
    // output_line(), which the TUI attaches to the current tool block,
    // this is always rendered as its own visible line.
    virtual void output_status(const std::string& text)
    {
      output_line(text);
    }
    // Resume preview: shows where a resumed session left off - the last
    // user message and the last assistant reply. The default renders a
    // plain status block; UIs with styled content override this to render
    // the user line and the assistant reply with their respective styles.
    virtual void output_resume_preview(const std::string& user_text, const std::string& assistant_text)
    {
      std::string out = "Last turn:\n  > " + user_text;
      if (!assistant_text.empty())
      {
        out += "\n  " + assistant_text;
      }
      output_status(out);
    }
    virtual void output_tool_call(const std::string& name, const std::string& args) = 0;
    virtual void output_tool_result(const std::string& result) = 0;
    virtual void output_error(const std::string& text) = 0;
    virtual void output_warning(const std::string& text) = 0;
    virtual void thinking_begin() = 0;
    virtual void thinking_end() = 0;
    virtual void completed() = 0;
    virtual void cancelled() = 0;
  };
} // namespace mistral::agent

#endif // MISTRAL_RENDER_SINK_H
