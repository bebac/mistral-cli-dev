#include "trace/trace.h"

#include <chrono>
#include <format>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <iostream>

namespace mistral::trace
{
  namespace
  {
    // Thread-local mutex for the logger instance
    inline std::mutex& logger_mutex() {
      static std::mutex mtx;
      return mtx;
    }
  }

  // Static instance
  logger& logger::instance() {
    static logger instance;
    return instance;
  }

  void logger::enable(bool enabled) {
    std::lock_guard<std::mutex> lock(logger_mutex());
    enabled_ = enabled;

    // Open file if enabling and path is set
    if (enabled_ && !log_path_.empty()) {
      // Don't open here - open on first write to avoid issues
    }
  }

  void logger::set_log_path(const std::string& path) {
    std::lock_guard<std::mutex> lock(logger_mutex());
    log_path_ = path;
  }

  std::string logger::category_to_string(category cat) const {
    switch (cat) {
      case category::connection: return "connection";
      case category::stream: return "stream";
      case category::flow_control: return "flow_control";
      case category::data: return "data";
      case category::sse: return "sse";
      case category::parse: return "parse";
      case category::policy: return "policy";
      case category::approval: return "approval";
      default: return "unknown";
    }
  }

  std::string logger::timestamp() const {
    auto now = std::chrono::system_clock::now();
    // %FT%T formats as YYYY-MM-DDTHH:MM:SS
    return std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::microseconds>(now));
  }

  void logger::log_impl(category cat, const std::source_location& loc, std::string msg) {
    std::lock_guard<std::mutex> lock(logger_mutex());

    if (log_path_.empty()) {
      // No path set, skip
      return;
    }

    // Build JSON line
    std::string line = std::format(
      "{{\"timestamp\":\"{}\",\"category\":\"{}\",\"file\":\"{}\",\"line\":{},\"function\":\"{}\",\"message\":\"{}\"}}",
      timestamp(), category_to_string(cat),
      loc.file_name(), loc.line(), loc.function_name(), msg);

    // Open file in append mode
    std::ofstream file(log_path_, std::ios::app);
    if (file.is_open()) {
      file << line << '\n';
      file.flush();
    }
}

} // namespace mistral::trace
