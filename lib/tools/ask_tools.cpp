#include "tools/ask_tools.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>

namespace mistral::tools
{
  void ask_tools::set_prompt_handler(prompt_handler_fn handler)
  {
    prompt_handler_ = std::move(handler);
  }

  std::string ask_tools::format_prompt(
    const std::string& question,
    const std::string& context,
    const std::vector<std::string>& options,
    const std::string& recommendation
  )
  {
    std::ostringstream oss;
    oss << "\n[QUESTION FROM AGENT]\n";
    oss << "  " << question << "\n";
    if (!context.empty())
    {
      oss << "\n  Context: " << context << "\n";
    }
    if (!options.empty())
    {
      oss << "\n  Options:\n";
      int i = 1;
      for (const auto& opt : options)
      {
        oss << "    " << i++ << ". " << opt << "\n";
      }
    }
    if (!recommendation.empty())
    {
      oss << "\n  Recommendation: " << recommendation << "\n";
    }
    if (!options.empty())
    {
      oss << "\n  Reply with the option number (1-based, as listed above) "
             "or the full option text.\n";
    }
    return oss.str();
  }

  std::optional<std::string> ask_tools::match_option(const std::string& answer, const std::vector<std::string>& options)
  {
    if (options.empty())
    {
      return std::nullopt;
    }

    // Trim surrounding whitespace.
    size_t first = answer.find_first_not_of(" \t\n\r");
    if (first == std::string::npos)
    {
      return std::nullopt;
    }
    size_t last = answer.find_last_not_of(" \t\n\r");
    std::string trimmed = answer.substr(first, last - first + 1);

    // The whole answer must be digits (no sign, no other text).
    if (!std::all_of(trimmed.begin(), trimmed.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
    {
      return std::nullopt;
    }

    int n = 0;
    auto [ptr, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), n);
    if (ec != std::errc() || ptr != trimmed.data() + trimmed.size())
    {
      return std::nullopt;
    }

    // Options are rendered 1-based; reject 0 and out-of-range values.
    if (n < 1 || static_cast<size_t>(n) > options.size())
    {
      return std::nullopt;
    }

    return options[static_cast<size_t>(n) - 1];
  }

  dripcore::awaitable<json> ask_tools::ask_user(
    std::string question,
    std::string context,
    std::vector<std::string> options,
    std::string recommendation
  )
  {
    if (!prompt_handler_)
    {
      co_return json{
        {"status", "unavailable"},
        {"message",
         "No human is available to answer right now. Proceed with your "
         "best judgment, state the assumption you made, and flag it "
         "clearly in your final response."}
      };
    }

    std::string answer = co_await prompt_handler_(
      std::string(ask_prompt_prefix) + format_prompt(question, context, options, recommendation)
    );

    if (answer.empty())
    {
      co_return json{
        {"status", "skipped"},
        {"message",
         "The human skipped this question (empty answer). Proceed with "
         "your best judgment, state the assumption you made, and flag "
         "it clearly in your final response."}
      };
    }

    // Resolve a bare numeric answer (1-based, matching the rendered
    // option list) to the option text, so a raw answer like "5" is never
    // ambiguous about 0- vs 1-indexing.
    json result{{"status", "answered"}, {"answer", answer}, {"matched_option", nullptr}};
    if (auto matched = match_option(answer, options))
    {
      result["matched_option"] = *matched;
    }
    co_return result;
  }
} // namespace mistral::tools
