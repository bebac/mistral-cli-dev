#ifndef MISTRAL_CONSOLE_RENDER_SINK_H
#define MISTRAL_CONSOLE_RENDER_SINK_H

#include "agent/render_sink.h"

#include <iostream>
#include <string>

namespace mistral::agent
{
  class console_render_sink : public render_sink
  {
  public:
    void output(const std::string& text) override
    {
      std::cout << normal_color_ << text << std::flush;
    }

    void output_thinking(const std::string& text) override
    {
      std::cout << thinking_color_ << text << std::flush;
    }

    void output_line(const std::string& text) override
    {
      std::cout << normal_color_ << text << std::endl;
    }

    void output_tool_call(const std::string& name,
                          const std::string& args) override
    {
      std::cout << tool_color_
                << "\n[tool] " << name
                << "(" << args << ")"
                << normal_color_ << std::flush;
    }

    void output_tool_result(const std::string& result) override
    {
      std::string display = (result.size() > 200)
        ? result.substr(0, 200) + "..."
        : result;
      std::cout << normal_color_ << " \xe2\x86\x92 " << display << std::endl;
    }

    void output_error(const std::string& text) override
    {
      std::cerr << "\n[error] " << text << std::endl;
    }

    void output_warning(const std::string& text) override
    {
      std::cerr << "\n[warning] " << text << std::endl;
    }

    void thinking_begin() override
    {
      std::cout << std::endl;
    }

    void thinking_end() override
    {
      std::cout << std::endl << std::endl;
    }

    void completed() override
    {
      std::cout << std::endl;
    }

    void cancelled() override
    {
      std::cout << "\n^C" << normal_color_ << std::endl;
    }

  private:
    static constexpr char normal_color_[]   = "\033[0m";
    static constexpr char thinking_color_[] = "\033[90m";
    static constexpr char tool_color_[]     = "\033[32m";
  };
} // namespace mistral::agent

#endif // MISTRAL_CONSOLE_RENDER_SINK_H