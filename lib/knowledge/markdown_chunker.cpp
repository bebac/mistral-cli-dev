#include "knowledge/markdown_chunker.h"

#include <cctype>
#include <sstream>
#include <utility>

namespace mistral::knowledge
{
  namespace
  {
    struct source_line
    {
      int number = 0; // 1-indexed
      std::string text;
    };

    std::vector<source_line> split_lines(const std::string& source)
    {
      std::vector<source_line> lines;
      std::istringstream stream(source);
      std::string text;
      int number = 0;
      while (std::getline(stream, text))
      {
        ++number;
        if (!text.empty() && text.back() == '\r')
        {
          text.pop_back();
        }
        lines.push_back({number, std::move(text)});
      }
      return lines;
    }

    std::string trimmed(const std::string& s)
    {
      std::size_t begin = 0;
      std::size_t end = s.size();
      while (begin < end && std::isspace(static_cast<unsigned char>(s[begin])))
      {
        ++begin;
      }
      while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])))
      {
        --end;
      }
      return s.substr(begin, end - begin);
    }

    // ATX header level (1-6) or 0 when the line is not a header.
    int header_level(const std::string& line)
    {
      std::size_t i = 0;
      while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
      {
        ++i;
      }
      if (i >= line.size() || line[i] != '#')
      {
        return 0;
      }
      int level = 0;
      while (i < line.size() && line[i] == '#')
      {
        ++level;
        ++i;
      }
      if (level > 6)
      {
        return 0;
      }
      // Require whitespace or end of line after the '#' run.
      if (i < line.size() && line[i] != ' ' && line[i] != '\t')
      {
        return 0;
      }
      return level;
    }

    std::string header_title(const std::string& line)
    {
      const std::size_t first_hash = line.find('#');
      std::string title = line.substr(first_hash);
      const std::size_t first_text = title.find_first_not_of('#');
      title = title.substr(first_text == std::string::npos ? title.size() : first_text);
      title = trimmed(title);
      // Strip an optional closing '#' run ("## Title ##").
      while (!title.empty() && title.back() == '#')
      {
        title.pop_back();
      }
      return trimmed(title);
    }

    // True when the line is a fenced code block marker (``` or ~~~).
    bool fence_marker(const std::string& line, char& fence_char, int& fence_len)
    {
      std::size_t i = 0;
      while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
      {
        ++i;
      }
      if (i + 3 <= line.size() && (line[i] == '`' || line[i] == '~'))
      {
        const char c = line[i];
        int len = 0;
        while (i + static_cast<std::size_t>(len) < line.size() && line[i + len] == c)
        {
          ++len;
        }
        if (len >= 3)
        {
          fence_char = c;
          fence_len = len;
          return true;
        }
      }
      return false;
    }

    bool is_blank(const std::string& line)
    {
      return trimmed(line).empty();
    }

    std::size_t text_size(const std::vector<source_line>& lines)
    {
      std::size_t size = 0;
      for (const auto& line : lines)
      {
        size += line.text.size() + 1;
      }
      return size;
    }
  } // namespace

  markdown_chunker::markdown_chunker(int max_chars) : max_chars_(max_chars > 0 ? max_chars : 1500)
  {
  }

  std::vector<doc_chunk> markdown_chunker::chunk(const std::string& source) const
  {
    std::vector<doc_chunk> chunks;
    const std::vector<source_line> lines = split_lines(source);

    std::vector<std::pair<int, std::string>> header_stack; // (level, title), outermost first
    std::vector<source_line> section; // lines since the last header (including it)
    char open_fence = 0;
    int open_fence_len = 0;
    bool frontmatter_open = false;
    bool frontmatter_checked = false;

    auto breadcrumb = [&]() {
      std::string result;
      for (const auto& [level, title] : header_stack)
      {
        (void)level;
        if (!result.empty())
        {
          result += " > ";
        }
        result += title;
      }
      return result;
    };

    auto emit = [&](const std::vector<source_line>& group, const std::string& bc) {
      std::string text;
      for (const auto& line : group)
      {
        text += line.text;
        text += '\n';
      }
      chunks.push_back(doc_chunk{"", bc, group.front().number, group.back().number, std::move(text)});
    };

    auto flush_section = [&]() {
      // Trim trailing blank lines so the line range ends at the last
      // content line (the blank line before the next header belongs to
      // neither section).
      while (!section.empty() && is_blank(section.back().text))
      {
        section.pop_back();
      }
      if (section.empty())
      {
        return;
      }

      // Skip header-only sections: a chunk that is just the header
      // line carries no content (the title lives on in the breadcrumbs
      // of the following sections), and under cosine similarity such a
      // near-empty chunk would outrank real content.
      if (section.size() == 1 && header_level(section[0].text) > 0)
      {
        return;
      }

      const std::string bc = breadcrumb();
      if (text_size(section) <= static_cast<std::size_t>(max_chars_))
      {
        emit(section, bc);
        section.clear();
        return;
      }

      // Oversized section: split at paragraph (blank-line) boundaries,
      // packing paragraphs greedily up to max_chars_. A single
      // paragraph larger than max_chars_ is split at line boundaries.
      std::vector<std::vector<source_line>> paragraphs;
      std::vector<source_line> paragraph;
      for (const auto& line : section)
      {
        if (is_blank(line.text))
        {
          if (!paragraph.empty())
          {
            paragraphs.push_back(paragraph);
            paragraph.clear();
          }
          continue;
        }
        paragraph.push_back(line);
      }
      if (!paragraph.empty())
      {
        paragraphs.push_back(paragraph);
      }

      // Merge tiny paragraphs (e.g. a lone header line before a blank
      // line) into the following paragraph: a chunk that is just a
      // title would otherwise outrank real content under cosine
      // similarity.
      constexpr std::size_t k_min_paragraph_chars = 100;
      std::vector<std::vector<source_line>> merged;
      merged.reserve(paragraphs.size());
      for (std::size_t i = 0; i < paragraphs.size(); ++i)
      {
        if (text_size(paragraphs[i]) < k_min_paragraph_chars && i + 1 < paragraphs.size())
        {
          std::vector<source_line> combined = std::move(paragraphs[i]);
          combined.insert(combined.end(), paragraphs[i + 1].begin(), paragraphs[i + 1].end());
          merged.push_back(std::move(combined));
          ++i;
          continue;
        }
        merged.push_back(std::move(paragraphs[i]));
      }

      std::vector<source_line> group;
      auto flush_group = [&]() {
        if (!group.empty())
        {
          emit(group, bc);
          group.clear();
        }
      };
      for (const auto& para : merged)
      {
        for (const auto& line : para)
        {
          if (!group.empty() &&
              text_size(group) + line.text.size() + 1 > static_cast<std::size_t>(max_chars_))
          {
            flush_group();
          }
          group.push_back(line);
        }
      }
      flush_group();
      section.clear();
    };

    for (const auto& line : lines)
    {
      // Skip a leading YAML frontmatter block (--- ... ---).
      if (!frontmatter_checked)
      {
        frontmatter_checked = true;
        if (trimmed(line.text) == "---")
        {
          frontmatter_open = true;
          continue;
        }
      }
      if (frontmatter_open)
      {
        if (trimmed(line.text) == "---")
        {
          frontmatter_open = false;
        }
        continue;
      }

      char fence_char = 0;
      int fence_len = 0;
      if (fence_marker(line.text, fence_char, fence_len))
      {
        if (open_fence == 0)
        {
          open_fence = fence_char;
          open_fence_len = fence_len;
        }
        else if (fence_char == open_fence && fence_len >= open_fence_len)
        {
          open_fence = 0;
          open_fence_len = 0;
        }
        section.push_back(line);
        continue;
      }

      if (open_fence == 0)
      {
        const int level = header_level(line.text);
        if (level > 0)
        {
          flush_section();
          while (!header_stack.empty() && header_stack.back().first >= level)
          {
            header_stack.pop_back();
          }
          header_stack.push_back({level, header_title(line.text)});
          section.push_back(line); // keep the header line in the chunk text
          continue;
        }
      }

      section.push_back(line);
    }
    flush_section();

    return chunks;
  }

} // namespace mistral::knowledge
