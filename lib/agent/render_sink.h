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
    virtual void output_tool_call(const std::string& name,
                                  const std::string& args) = 0;
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