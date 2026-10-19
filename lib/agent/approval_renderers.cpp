#include "agent/approval_renderers.h"

#include "agent/agent_system.h"
#include "agent/tool_registry.h"
#include "tools/edit_types.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>

namespace mistral::agent
{
  namespace
  {
    // Maximum number of edits listed in a patchset_apply approval prompt
    // before the list is truncated.
    constexpr size_t max_shown_edits = 10;

    // Count lines in a text block. A trailing newline does not start a
    // new line.
    size_t line_count(std::string_view text)
    {
      if (text.empty()) return 0;
      size_t count = 1;
      for (char c : text)
      {
        if (c == '\n') ++count;
      }
      if (text.back() == '\n') --count;
      return count;
    }

    std::string line_count_suffix(std::string_view text)
    {
      size_t count = line_count(text);
      return " (" + std::to_string(count) + (count == 1 ? " line)" : " lines)");
    }

    // One line per edit: patch number, status letter, file path.
    //   M = edit (search/replace)
    //   A = new file (search_text empty)
    //   O = overwrite entire file (search_text == ".")
    //   D = delete (replace_text empty)
    std::string render_patchset_apply_approval(const json& args, std::string_view reason)
    {
      std::string patchset_id;
      if (args.contains("patchset_id") && args["patchset_id"].is_string())
      {
        patchset_id = args["patchset_id"].get<std::string>();
      }

      std::ostringstream oss;
      oss << "\n[APPROVAL REQUIRED] patchset_apply\n\n";

      auto patchset = agent_system::instance().get_patchset(patchset_id);
      if (!patchset)
      {
        oss << "  patchset: " << patchset_id << " (not found)\n";
      }
      else
      {
        const auto& edits = patchset->edits;
        oss << "  patchset: " << patchset_id << " ("
            << edits.size() << (edits.size() == 1 ? " edit" : " edits") << ")\n";

        size_t shown = std::min(edits.size(), max_shown_edits);
        for (size_t i = 0; i < shown; ++i)
        {
          const auto& edit = edits[i];
          char status = 'M';
          std::string suffix;
          if (edit.search_text.empty())
          {
            status = 'A'; // new file
            suffix = line_count_suffix(edit.replace_text);
          }
          else if (edit.search_text == ".")
          {
            status = 'O'; // overwrite entire file
            suffix = line_count_suffix(edit.replace_text);
          }
          else if (edit.replace_text.empty())
          {
            status = 'D'; // delete matched text
          }
          oss << "    " << edit.patch_number << ". " << status << ' '
              << edit.file_path << suffix << "\n";
        }
        if (edits.size() > max_shown_edits)
        {
          oss << "    ... and " << (edits.size() - max_shown_edits)
              << " more edits\n";
        }
      }

      oss << "\n  Reason: " << reason << "\n";
      oss << "  Approve? [y/n/r/a/?]: ";
      return oss.str();
    }

    std::string render_exec_approval(const json& args, std::string_view reason)
    {
      std::ostringstream cmd_line;
      if (args.contains("command") && args["command"].is_string())
      {
        cmd_line << args["command"].get<std::string>();
      }
      if (args.contains("args") && args["args"].is_array())
      {
        for (const auto& a : args["args"])
        {
          if (a.is_string())
          {
            cmd_line << ' ' << a.get<std::string>();
          }
        }
      }

      std::ostringstream oss;
      oss << "\n[APPROVAL REQUIRED] exec\n\n";
      oss << "  Command: " << cmd_line.str() << "\n";
      if (args.contains("cwd") && args["cwd"].is_string())
      {
        std::string cwd = args["cwd"].get<std::string>();
        if (!cwd.empty())
        {
          oss << "  Working directory: " << cwd << "\n";
        }
      }
      oss << "\n  Reason: " << reason << "\n";
      oss << "  Approve? [y/n/r/a/?]: ";
      return oss.str();
    }
  } // namespace

  void register_approval_renderers(tool_registry& tools)
  {
    tools.register_approval_renderer(
      "patchset_apply",
      [](const json& args, std::string_view reason) {
        return render_patchset_apply_approval(args, reason);
      }
    );
    tools.register_approval_renderer(
      "exec",
      [](const json& args, std::string_view reason) {
        return render_exec_approval(args, reason);
      }
    );
  }
} // namespace mistral::agent
