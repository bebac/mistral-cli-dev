#ifndef MISTRAL_TUI_TYPES_H
#define MISTRAL_TUI_TYPES_H

#include <functional>
#include <string>

namespace mistral::tui
{
  // ---------------------------------------------------------------------------
  // Content type tag for styled rendering. Used by the render sink, bridge
  // tasks, content area, and content log to tell the TUI which color/style
  // to apply to a fragment.
  // ---------------------------------------------------------------------------
  enum class content_type
  {
    normal,        // Regular AI response text
    thinking,      // AI thinking/reasoning
    tool_call,     // Tool call header (e.g. "[tool] write(...)")
    tool_result,   // Tool result or status (e.g. "→ result")
    tool_approval, // Tool approval prompt (attached to last tool block)
    error,         // Error messages
    warning,       // Warning messages
    user,          // User input echo
    system,        // System messages (non-approval)
  };

  // ---------------------------------------------------------------------------
  // Shared callback type aliases used across the TUI layer:
  //   - tui_render_sink (producer, dripcore thread)
  //   - tui_bridge_tasks (bridge, dripcore thread → UI thread)
  //   - tui_backend::ui_callbacks (central wiring point)
  //   - tui_app (consumer, UI thread)
  //
  // Defining these in one place ensures signature changes are caught at
  // compile time across all consumers.
  // ---------------------------------------------------------------------------
  using post_fn = std::function<void(std::function<void()>)>;
  using append_fn = std::function<void(const std::string&, content_type)>;
  using flush_fn = std::function<void()>;
  using completed_fn = std::function<void()>;
} // namespace mistral::tui

#endif // MISTRAL_TUI_TYPES_H
