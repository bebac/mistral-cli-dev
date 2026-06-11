#include "template/template_engine.h"

#include <array>
#include <cstdio>
#include <format>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace mistral::tmpl
{
  std::string template_engine::process(const std::string& input) const
  {
    std::set<std::string> include_stack;
    return process_impl(input, include_stack);
  }

  std::string template_engine::process_file(const fs::path& path) const
  {
    auto content = read_file(path);
    std::set<std::string> include_stack;
    include_stack.insert(fs::canonical(path).string());
    return process_impl(content, include_stack);
  }

  std::string template_engine::process_impl(const std::string& input,
                                             std::set<std::string>& include_stack) const
  {
    std::string result;
    result.reserve(input.size());

    size_t pos = 0;
    size_t len = input.size();

    while (pos < len)
    {
      // {{> path}} — file inclusion (must check before ${{ to avoid ambiguity).
      if (pos + 3 < len && input.compare(pos, 4, "{{> ") == 0)
      {
        size_t end = input.find("}}", pos + 4);
        if (end == std::string::npos) {
          throw std::runtime_error("unclosed include directive: {{> ...");
        }

        std::string path = input.substr(pos + 4, end - (pos + 4));
        // Trim whitespace.
        while (!path.empty() && path.back() == ' ') path.pop_back();
        while (!path.empty() && path.front() == ' ') path.erase(path.begin());

        result += expand_include(path, include_stack);
        pos = end + 2;
      }
      // ${{command}} — shell command expansion.
      else if (pos + 2 < len && input.compare(pos, 3, "${{") == 0)
      {
        size_t end = input.find("}}", pos + 3);
        if (end == std::string::npos) {
          throw std::runtime_error("unclosed command expansion: ${{...");
        }

        std::string cmd = input.substr(pos + 3, end - (pos + 3));
        result += expand_command(cmd);
        pos = end + 2;
      }
      // ${variable} — variable substitution.
      else if (pos + 1 < len && input[pos] == '$' && input[pos + 1] == '{')
      {
        size_t end = input.find('}', pos + 2);
        if (end == std::string::npos) {
          throw std::runtime_error("unclosed variable: ${...");
        }

        std::string name = input.substr(pos + 2, end - (pos + 2));
        result += expand_variable(name);
        pos = end + 1;
      }
      // Regular character.
      else
      {
        result += input[pos];
        ++pos;
      }
    }

    return result;
  }

  std::string template_engine::expand_variable(const std::string& name) const
  {
    auto it = variables_.find(name);
    if (it != variables_.end()) {
      return it->second;
    }
    // Fall back to environment variable.
    const char* env = std::getenv(name.c_str());
    return env ? std::string(env) : "";
  }

  std::string template_engine::expand_command(const std::string& command) const
  {
    if (!command_expansion_) {
      return std::format("${{{{ {} }}}}", command);  // Return unexpanded.
    }

    std::array<char, 256> buffer;
    std::string output;

    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
    if (!pipe) {
      throw std::runtime_error(std::format("failed to execute: {}", command));
    }

    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
      output += buffer.data();
    }

    // Trim trailing newline.
    if (!output.empty() && output.back() == '\n') {
      output.pop_back();
    }

    return output;
  }

  std::string template_engine::expand_include(const std::string& path_str,
                                               std::set<std::string>& include_stack) const
  {
    if (include_base_.empty()) {
      throw std::runtime_error("include base directory not set");
    }

    fs::path target = include_base_ / path_str;

    // Append .md if the path has no extension.
    if (!target.has_extension()) {
      target += ".md";
    }

    if (!fs::exists(target)) {
      throw std::runtime_error(std::format("include not found: {}", target.string()));
    }

    std::string canonical = fs::canonical(target).string();

    if (include_stack.contains(canonical)) {
      throw std::runtime_error(std::format("circular include detected: {}", path_str));
    }

    include_stack.insert(canonical);
    auto content = read_file(target);
    auto expanded = process_impl(content, include_stack);
    include_stack.erase(canonical);

    return expanded;
  }

  std::string template_engine::read_file(const fs::path& path)
  {
    std::ifstream f(path);
    if (!f) {
      throw std::runtime_error(std::format("cannot read file: {}", path.string()));
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }

} // namespace mistral::tmpl
