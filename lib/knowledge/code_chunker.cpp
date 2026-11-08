#include "knowledge/code_chunker.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <utility>

namespace mistral::knowledge
{
  namespace
  {
    // Longest breadcrumb kept. A declaration line is normally short; a
    // pathological one must not dominate the embedded text.
    constexpr std::size_t k_max_breadcrumb_chars = 200;

    // A line that opens a top-level declaration in one of the languages
    // we index. Deliberately loose: a false positive only makes a
    // breadcrumb slightly less useful, and \b keeps "default" from
    // matching "def".
    const std::regex& declaration_re()
    {
      static const std::regex re(
        R"(^\s*(?:template\s*<[^>]*>\s*)?)"
        R"((?:class|struct|union|namespace|enum|interface|impl|trait|module|def|func|function|fn|sub|package)\b)"
      );
      return re;
    }

    // A scope opened by a declaration. `opened` becomes true once the
    // body has been entered, which is what distinguishes "declaration
    // whose brace is on the next line" from "scope already closed".
    struct scope_entry
    {
      int depth = 0;
      std::string name;
      bool opened = false;
    };

    // Net brace count on a line. Naive: braces inside strings and
    // comments are counted too. A breadcrumb only has to be roughly
    // right, and a real lexer per language is not worth it here.
    int brace_delta(const std::string& line)
    {
      int delta = 0;
      for (const char c : line)
      {
        if (c == '{')
        {
          ++delta;
        }
        else if (c == '}')
        {
          --delta;
        }
      }
      return delta;
    }

    // Join the scope path, dropping outermost entries if it grows past
    // the budget: the innermost scope is the most specific and the one
    // worth keeping.
    std::string join_scopes(const std::vector<scope_entry>& scopes)
    {
      std::size_t total = 0;
      for (std::size_t i = 0; i < scopes.size(); ++i)
      {
        total += scopes[i].name.size() + (i == 0 ? 0 : 3);
      }
      std::size_t start = 0;
      while (start + 1 < scopes.size() && total > k_max_breadcrumb_chars)
      {
        total -= scopes[start].name.size() + 3;
        ++start;
      }

      std::string out;
      for (std::size_t i = start; i < scopes.size(); ++i)
      {
        if (!out.empty())
        {
          out += " > ";
        }
        out += scopes[i].name;
      }
      return out;
    }

    bool is_blank(const std::string& line)
    {
      return std::all_of(line.begin(), line.end(), [](unsigned char c) { return std::isspace(c) != 0; });
    }

    std::string trim(const std::string& s)
    {
      const auto begin = s.find_first_not_of(" \t\r\n");
      if (begin == std::string::npos)
      {
        return {};
      }
      const auto end = s.find_last_not_of(" \t\r\n");
      return s.substr(begin, end - begin + 1);
    }

    // Split on '\n', dropping a trailing '\r' so CRLF files chunk the
    // same way as LF ones.
    std::vector<std::string> split_lines(const std::string& content)
    {
      std::vector<std::string> lines;
      std::size_t start = 0;
      while (start < content.size())
      {
        const std::size_t nl = content.find('\n', start);
        if (nl == std::string::npos)
        {
          lines.push_back(content.substr(start));
          break;
        }
        std::string line = content.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r')
        {
          line.pop_back();
        }
        lines.push_back(std::move(line));
        start = nl + 1;
      }
      return lines;
    }
  } // namespace

  code_chunker::code_chunker(int max_lines, int overlap_lines, int max_chars)
    : max_lines_(std::max(1, max_lines)),
      overlap_lines_(std::clamp(overlap_lines, 0, std::max(0, max_lines - 1))),
      max_chars_(std::max(1, max_chars))
  {
  }

  std::vector<doc_chunk> code_chunker::chunk(const std::string& content) const
  {
    std::vector<doc_chunk> chunks;
    if (content.empty())
    {
      return chunks;
    }

    const std::vector<std::string> lines = split_lines(content);
    if (lines.empty())
    {
      return chunks;
    }

    // Breadcrumb per line: the enclosing scope path at that point.
    // Accumulated rather than replaced — taking only the last
    // declaration line meant a nested or anonymous namespace hid the
    // namespace it lived in, so an anonymous namespace inside
    // `namespace mistral::knowledge` produced the breadcrumb
    // "namespace", which tells a reader nothing.
    std::vector<std::string> breadcrumbs;
    breadcrumbs.reserve(lines.size());
    std::vector<scope_entry> scopes;
    int depth = 0;
    for (const auto& line : lines)
    {
      const int delta = brace_delta(line);
      if (std::regex_search(line, declaration_re()))
      {
        scopes.push_back({depth, trim(line).substr(0, k_max_breadcrumb_chars), false});
        // A declaration that opens and closes on one line (struct foo
        // {};) introduces no scope.
        if (delta == 0 && line.find('{') != std::string::npos)
        {
          scopes.pop_back();
        }
      }

      depth += delta;
      if (depth < 0)
      {
        depth = 0;
      }

      // Retire scopes whose body has ended: entered, and depth is back
      // to where the declaration sat.
      while (!scopes.empty())
      {
        scope_entry& back = scopes.back();
        if (depth > back.depth)
        {
          back.opened = true;
          break;
        }
        if (back.opened)
        {
          scopes.pop_back();
          continue;
        }
        break;
      }

      breadcrumbs.push_back(join_scopes(scopes));
    }

    const int total = static_cast<int>(lines.size());
    const int step = std::max(1, max_lines_ - overlap_lines_);
    int start = 0;

    while (start < total)
    {
      int end = std::min(start + max_lines_, total);

      // Snap the end to just after a blank line so a window does not cut
      // a function in half. Only the back half is searched: snapping
      // from further back would make the windows tiny.
      if (end < total)
      {
        const int floor_line = start + (end - start) / 2;
        for (int i = end - 1; i >= floor_line; --i)
        {
          if (is_blank(lines[static_cast<std::size_t>(i)]))
          {
            end = i + 1;
            break;
          }
        }
      }

      // Build the window, stopping at the character budget.
      std::string text;
      int actual_end = start;
      for (int i = start; i < end; ++i)
      {
        const std::string& line = lines[static_cast<std::size_t>(i)];
        if (actual_end > start && text.size() + line.size() + 1 > static_cast<std::size_t>(max_chars_))
        {
          break;
        }
        text += line;
        text += '\n';
        ++actual_end;
      }

      if (actual_end > start && !trim(text).empty())
      {
        doc_chunk chunk;
        chunk.breadcrumb = start > 0 ? breadcrumbs[static_cast<std::size_t>(start - 1)] : std::string{};
        chunk.line_start = start + 1;
        chunk.line_end = actual_end;
        chunk.text = std::move(text);
        chunks.push_back(std::move(chunk));
      }

      if (actual_end >= total)
      {
        break;
      }
      // Never skip lines: when the character budget cut the window
      // short, resume exactly where it stopped.
      start = actual_end < end ? actual_end : std::min(start + step, end);
    }

    return chunks;
  }

} // namespace mistral::knowledge