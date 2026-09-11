#ifndef MISTRAL_TUI_APPROVAL_STATE_H
#define MISTRAL_TUI_APPROVAL_STATE_H

#include <functional>

namespace mistral::tui
{
  // ---------------------------------------------------------------------------
  // Approval state machine for the TUI's tool approval flow.
  //
  // idle:        No approval pending. Normal chat input active.
  // single_key:  Agent is waiting for y/n/r/a/? approval response.
  // text_input:  User pressed 'r' (reject with message); agent is
  //              waiting for the rejection message text.
  //
  // State transitions are event-driven: prompt_bridge_task posts
  // transitions to the UI thread via App::post(), eliminating the
  // former 50ms polling timer and the dual atomic/enum sync.
  // ---------------------------------------------------------------------------
  enum class approval_state {
    idle,
    single_key,
    text_input,
  };

  // Callback type for posting approval state transitions from the
  // dripcore thread to the UI thread. The callback is invoked inside
  // a posted App::post() lambda, so it always runs on the UI thread.
  using approval_state_fn = std::function<void(approval_state)>;
}

#endif // MISTRAL_TUI_APPROVAL_STATE_H