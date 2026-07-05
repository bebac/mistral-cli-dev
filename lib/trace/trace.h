#ifndef MISTRAL_TRACE_TRACE_H
#define MISTRAL_TRACE_TRACE_H

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <format>
#include <source_location>
#include <type_traits>

namespace mistral::trace
{
  // Trace categories for different subsystems
  enum class category {
    connection,     // Connection lifecycle (TLS, HTTP/2 preface)
    stream,         // Stream creation/destruction
    flow_control,   // WINDOW_UPDATE, SETTINGS frames
    data,           // Raw data received/sent
    sse,            // SSE parser input/output
    parse,          // JSON parsing results
    policy,         // Policy engine decisions
    approval        // Approval prompts
  };

  // The main logger class
  class logger
  {
  public:
    static logger& instance();

    // Runtime configuration
    void enable(bool enabled = true);
    void disable() { enable(false); }
    bool enabled() const { return enabled_; }

    void set_log_path(const std::string& path);

    // Main trace method with formatting
    template <typename... Args>
    struct trace_format {
      std::format_string<Args...> fmt;
      std::source_location loc;

      template <typename T>
        requires std::convertible_to<T, std::format_string<Args...>>
      constexpr trace_format(const T& s, std::source_location l = std::source_location::current())
        : fmt(s), loc(l) {}
    };

    // Use std::type_identity_t to block deduction on the second argument
    template <typename... Args>
    void trace(category cat, std::type_identity_t<trace_format<Args...>> target, Args&&... args)
    {
      if ( enabled_ )
      {
        log_impl(
          cat,
          target.loc,
          std::vformat(target.fmt.get(), std::make_format_args(args...))
        );
      }
    }

  private:
    void log_impl(category cat, const std::source_location& loc, std::string msg);

  public:
    std::string category_to_string(category cat) const;
    std::string timestamp() const;

  private:
    std::string log_path_;
    bool enabled_ = false;
  };

} // namespace mistral::trace

// Custom formatter for std::span<const std::byte>
namespace std
{
  template <size_t Extent>
  struct formatter<span<const byte, Extent>> {
    constexpr auto parse(format_parse_context& ctx) {
      return ctx.begin();
    }

    auto format(span<const byte, Extent> bytes, format_context& ctx) const {
      auto out = ctx.out();
      out = format_to(out, "[");
      for (size_t i = 0; i < bytes.size(); ++i) {
        out = format_to(out, "{:02x}", to_integer<unsigned char>(bytes[i]));
        if (i + 1 < bytes.size()) {
          out = format_to(out, ", ");
        }
      }
      return format_to(out, "]");
    }
  };

  // Custom formatter for std::span<byte, Extent> (non-const)
  template <size_t Extent>
  struct formatter<span<byte, Extent>> : formatter<span<const byte, Extent>> {
    auto format(span<byte, Extent> bytes, format_context& ctx) const {
      return formatter<span<const byte, Extent>>::format(
          {bytes.data(), bytes.size()}, ctx);
    }
  };
}

#endif // MISTRAL_TRACE_TRACE_H
