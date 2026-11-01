#ifndef MISTRAL_AGENT_PROMPT_MESSAGE_H
#define MISTRAL_AGENT_PROMPT_MESSAGE_H

#include <string>

namespace mistral::agent
{
  /**
   * @brief A prompt sent from an agent task to the frontend.
   *
   * Replaces the former "[APPROVAL_PROMPT]" / "[ASK_USER]" string prefixes,
   * which every frontend had to parse independently. The producer knows what
   * kind of prompt it is emitting, so it tags it here and the consumers just
   * switch on the tag.
   */
  struct prompt_message
  {
    enum class kind
    {
      ready,    ///< The agent is ready for input.
      approval, ///< Approval prompt: single-key response.
      ask,      ///< ask_user question: free-text answer.
      text,     ///< Plain text prompt.
    };

    kind type = kind::text;

    /// The prompt text. For `ready` this is the prompt string the console
    /// hands to readline; the TUI only uses the tag.
    std::string text;
  };
} // namespace mistral::agent

#endif // MISTRAL_AGENT_PROMPT_MESSAGE_H
