#ifndef MISTRAL_ASK_TOOLS_H
#define MISTRAL_ASK_TOOLS_H

#include <dripcore/awaitable.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mistral::tools
{
  using json = nlohmann::ordered_json;

  // Transport prefix for ask prompts on the prompt channel. The console
  // and TUI prompt consumers recognize it to render the question and
  // switch the input into free-text answer mode.
  inline constexpr std::string_view ask_prompt_prefix = "[ASK_USER]";

  // Human-in-the-loop question tool. When the agent hits a decision point
  // that changes the direction of the work, it calls ask_user with the
  // question, the discovered context, the viable options and a
  // recommendation. The tool blocks until the human answers; the answer is
  // returned to the model as the tool result.
  //
  // The tool itself has no channel access: the interactive agent task
  // registers a prompt handler (set_prompt_handler) that renders the
  // question to the human and suspends until the answer arrives. When no
  // handler is registered (non-interactive contexts), ask_user returns an
  // "unavailable" result instead of blocking forever.
  class ask_tools
  {
  public:
    // Renders the prompt message to the human and suspends until the
    // answer arrives. Returns the answer, or an empty string when the
    // human skipped (empty answer) or the input channel closed.
    using prompt_handler_fn = std::function<dripcore::awaitable<std::string>(const std::string& prompt)>;

    // Register the prompt handler (called by the interactive agent task).
    void set_prompt_handler(prompt_handler_fn handler);

    // Ask the human a question. Blocks until answered.
    //   question       - the decision that needs a human answer
    //   context        - what was discovered that forces the decision
    //   options        - viable options, one per entry
    //   recommendation - the agent's recommended option and why
    dripcore::awaitable<json> ask_user(
      std::string question,
      std::string context,
      std::vector<std::string> options,
      std::string recommendation
    );

  private:
    // Format the question block sent to the human (without the
    // transport prefix, which is prepended by ask_user).
    static std::string format_prompt(
      const std::string& question,
      const std::string& context,
      const std::vector<std::string>& options,
      const std::string& recommendation
    );

    // If the answer is a bare integer in [1, options.size()], return the
    // corresponding option text (options are rendered 1-based). Otherwise
    // return nullopt.
    static std::optional<std::string> match_option(const std::string& answer, const std::vector<std::string>& options);

    prompt_handler_fn prompt_handler_;
  };

} // namespace mistral::tools

#endif // MISTRAL_ASK_TOOLS_H
