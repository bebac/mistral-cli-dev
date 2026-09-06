// content_log_demo: standalone TUI app that loads a saved chat log
// and displays it using the ContentLog widget inside a ScrollableVertical.
// Content is streamed incrementally via a timer to simulate the real
// TUI's network streaming behavior.
//
// Usage:
//   ./build/content_log_demo [path/to/chat.json]
//   ./build/content_log_demo  (defaults to .mistral-cli/chats/chat.latest.json)
//
// Keys:
//   ↑/↓       line scroll
//   PgUp/PgDn page scroll
//   Tab       toggle collapse/expand block at viewport top
//   q         quit
//

#include "tui/content_log.h"
#include "tui/tui_widgets.h"

#include <cpptui.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using content_type = mistral::agent::content_type;

// ---------------------------------------------------------------------------
// stream_fragment: a single chunk of text to be fed into ContentLog.
// force_new controls whether a new block is created even if the last block
// has the same kind — used to separate individual tool calls/results.
// ---------------------------------------------------------------------------
enum class special_type { none, turn_separator };

struct stream_fragment {
  std::string text;
  content_type type;
  bool force_new;
  special_type special = special_type::none;
  int turn_number = 0;
};

// ---------------------------------------------------------------------------
// Break a text string into smaller chunks for streaming simulation.
// Each chunk is at most chunk_size characters, split on word boundaries
// when possible. The first chunk gets force_new=true, rest get false.
// ---------------------------------------------------------------------------
static void add_text_chunks(std::vector<stream_fragment>& queue,
                            const std::string& text,
                            content_type type,
                            std::size_t chunk_size = 80)
{
  if (text.empty()) return;
  std::size_t pos = 0;
  bool first = true;
  while (pos < text.size()) {
    std::size_t end = std::min(pos + chunk_size, text.size());
    // Try to break on a word boundary
    if (end < text.size()) {
      std::size_t space = text.rfind(' ', end);
      if (space != std::string::npos && space > pos) {
        end = space + 1;
      }
    }
    queue.push_back({text.substr(pos, end - pos), type, first,
                     special_type::none, 0});
    first = false;
    pos = end;
  }
}

// ---------------------------------------------------------------------------
// Extract a plain-text string from a message's "content" field, which can
// be either a JSON string or an array of message_content objects.
// ---------------------------------------------------------------------------
static std::string extract_content_string(const nlohmann::json& content)
{
  if (content.is_string()) return content.get<std::string>();
  if (content.is_array()) {
    std::string result;
    for (const auto& mc : content) {
      std::string type = mc.value("type", "text");
      if (type == "text") {
        result += mc.value("text", "");
      }
    }
    return result;
  }
  return "";
}

// ---------------------------------------------------------------------------
// Parse a chat log JSON file into a queue of stream_fragments.
// Each tool call and tool result becomes its own block (force_new=true)
// so they can be individually collapsed/expanded in the future.
// ---------------------------------------------------------------------------
static bool build_fragment_queue(std::vector<stream_fragment>& queue,
                                 const std::string& path)
{
  std::ifstream in(path);
  if (!in) {
    std::cerr << "Cannot open: " << path << "\n";
    return false;
  }

  nlohmann::json data;
  try {
    data = nlohmann::json::parse(in);
  } catch (const std::exception& e) {
    std::cerr << "JSON parse error: " << e.what() << "\n";
    return false;
  }

  if (!data.is_array()) {
    std::cerr << "Expected JSON array at top level\n";
    return false;
  }

  // Build a map of tool_call_id → result text from all tool messages.
  // This lets us interleave each tool_call with its matching result.
  std::map<std::string, std::string> tool_results;
  for (const auto& msg : data) {
    if (msg.value("role", "") == "tool") {
      std::string id = msg.value("tool_call_id", "");
      std::string text = extract_content_string(msg["content"]);
      tool_results[id] = text;
    }
  }

  int turn_number = 0;

  for (const auto& msg : data) {
    std::string role = msg.value("role", "");

    if (role == "system") {
      continue;
    }

    if (role == "user") {
      turn_number++;
      // Emit turn separator before each user message.
      queue.push_back({"", content_type::system, true,
                       special_type::turn_separator, turn_number});
      std::string text = extract_content_string(msg["content"]);
      add_text_chunks(queue, "\n> " + text + "\n", content_type::user);
    }
    else if (role == "assistant") {
      // Content can be a string or an array of message_content objects.
      if (msg.contains("content")) {
        const auto& content = msg["content"];
        if (content.is_string()) {
          if (!content.get<std::string>().empty()) {
            // Blank line before final answer.
            add_text_chunks(queue, "\n", content_type::normal);
            add_text_chunks(queue, content.get<std::string>(),
                            content_type::normal);
          }
        }
        else if (content.is_array()) {
          for (const auto& mc : content) {
            std::string type = mc.value("type", "text");
            if (type == "thinking") {
              std::string thinking_text;
              const auto& thinking = mc["thinking"];
              if (thinking.is_string()) {
                thinking_text = thinking.get<std::string>();
              }
              else if (thinking.is_array() && !thinking.empty()) {
                thinking_text = thinking[0].value("text", "");
              }
              // Surround thinking with newlines to separate from text.
              add_text_chunks(queue, "\n", content_type::normal);
              add_text_chunks(queue, thinking_text, content_type::thinking);
              add_text_chunks(queue, "\n", content_type::normal);
            }
            else if (type == "text") {
              std::string text = mc.value("text", "");
              if (!text.empty()) {
                // Blank line before final answer.
                add_text_chunks(queue, "\n", content_type::normal);
                add_text_chunks(queue, text, content_type::normal);
              }
            }
          }
        }
      }

      // Tool calls — each one creates a new tool block, immediately
      // followed by its matching result (interleaved by tool_call_id).
      if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
        for (const auto& tc : msg["tool_calls"]) {
          std::string name = tc["function"].value("name", "");
          std::string args = tc["function"].value("arguments", "");
          std::string id = tc.value("id", "");
          queue.push_back({
            "\n[tool] " + name + "(" + args + ")",
            content_type::tool_call,
            true
          });
          // Emit matching result immediately after the call.
          auto it = tool_results.find(id);
          if (it != tool_results.end()) {
            add_text_chunks(queue, " \xe2\x86\x92 " + it->second + "\n",
                            content_type::tool_result, 200);
          }
        }
      }
    }
    // Tool role messages are consumed via the tool_results map above.
  }

  return true;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[])
{
  std::string chat_path = ".mistral-cli/chats/chat.latest.json";
  if (argc > 1) chat_path = argv[1];

  // Use light theme.
  cpptui::Theme::set_theme(cpptui::Theme::Light());

  // Build the widget tree.
  cpptui::App app;

  auto content_log = std::make_shared<mistral::tui::ContentLog>();

  // Parse chat log into a fragment queue (no ContentLog interaction yet).
  std::vector<stream_fragment> queue;
  if (!build_fragment_queue(queue, chat_path)) {
    return 1;
  }

  auto scroll = std::make_shared<cpptui::ScrollableVertical>();
  scroll->add(content_log);
  scroll->focusable = false;

  auto divider_top = std::make_shared<Divider>();
  auto divider_bottom = std::make_shared<Divider>();

  auto footer = std::make_shared<FooterBar>();
  footer->left_text = "\xe2\x86\x91\xe2\x86\x93 scroll \xc2\xb7 PgUp/PgDn page \xc2\xb7 Tab collapse";
  footer->right_text = "q quit";
  // Show just the filename, not the full path.
  auto pos = chat_path.find_last_of('/');
  footer->center_text = (pos != std::string::npos)
    ? chat_path.substr(pos + 1) : chat_path;
  footer->fg_color = cpptui::Theme::current().panel_fg;
  footer->bg_color = cpptui::Theme::current().panel_bg;

  auto root = std::make_shared<cpptui::Vertical>();
  root->add(divider_top);
  root->add(scroll);
  root->add(divider_bottom);
  root->add(footer);

  // Scroll helpers.
  auto clamp_scroll = [&]() {
    if (scroll->scroll_offset < 0) scroll->scroll_offset = 0;
    int visible_h = scroll->height > 0 ? scroll->height : 1;
    int max_scroll = scroll->content_height - visible_h;
    if (max_scroll < 0) max_scroll = 0;
    if (scroll->scroll_offset > max_scroll)
      scroll->scroll_offset = max_scroll;
  };

  auto scroll_to_bottom = [&]() {
    int visible_h = scroll->height > 0 ? scroll->height : 1;
    int max_scroll = scroll->content_height - visible_h;
    if (max_scroll < 0) max_scroll = 0;
    scroll->scroll_offset = max_scroll;
  };

  // Keybindings.
  // Up/Down — line scroll.
  app.register_key(1065, [&]() {
    scroll->scroll_offset -= 1;
    clamp_scroll();
  });
  app.register_key(1066, [&]() {
    scroll->scroll_offset += 1;
    clamp_scroll();
  });

  // PgUp/PgDn — page scroll.
  app.register_key(1001, [&]() {
    int visible_h = scroll->height > 0 ? scroll->height : 1;
    scroll->scroll_offset -= visible_h;
    clamp_scroll();
  });
  app.register_key(1002, [&]() {
    int visible_h = scroll->height > 0 ? scroll->height : 1;
    scroll->scroll_offset += visible_h;
    clamp_scroll();
  });

  // Tab — toggle collapse/expand block at viewport top.
  app.register_key(9, [&]() {
    int block_idx = content_log->block_at_line(scroll->scroll_offset);
    if (block_idx >= 0) {
      content_log->toggle_collapse(block_idx);
      content_log->flush();
      if (auto cont =
            std::dynamic_pointer_cast<cpptui::Container>(root)) {
        cont->layout();
      }
      clamp_scroll();
      app.update();
    }
  });

  // q — quit.
  app.register_exit_key('q');
  // Ctrl+C — quit.
  app.register_exit_key(3, true);

  // Streaming simulation: feed fragments from the queue incrementally.
  // A single recurring timer handles both the initial layout wait and the
  // streaming phase. We cannot add/remove timers from within timer
  // callbacks — that would modify the timers_ vector while the event loop
  // is iterating over it (undefined behavior).
  //
  // Phase 0: wait for first layout (width assigned), then flush + scroll.
  // Phase 1: feed fragments from the queue, a few per tick.
  // Phase 2: queue empty — timer becomes a no-op.
  int stream_phase = 0;

  app.add_timer(30, [&]() {
    if (stream_phase == 0) {
      // Wait for layout to assign width to content_log.
      if (content_log->width <= 0) return;
      stream_phase = 1;
    }

    if (stream_phase == 1) {
      // Feed a few fragments per tick to simulate streaming.
      constexpr int fragments_per_tick = 5;
      for (int i = 0; i < fragments_per_tick && !queue.empty(); ++i) {
        auto frag = queue.front();
        queue.erase(queue.begin());
        // Handle special fragment types.
        if (frag.special == special_type::turn_separator) {
          content_log->append_turn_separator(frag.turn_number);
        } else {
          content_log->append_fragment(frag.text, frag.type, frag.force_new);
        }
      }
      content_log->flush();
      if (auto cont =
            std::dynamic_pointer_cast<cpptui::Container>(root)) {
        cont->layout();
      }
      scroll_to_bottom();
      app.update();

      if (queue.empty()) {
        content_log->flush();
        if (auto cont =
              std::dynamic_pointer_cast<cpptui::Container>(root)) {
          cont->layout();
        }
        scroll_to_bottom();
        app.update();
        stream_phase = 2;
      }
    }
    // Phase 2: no-op, timer keeps firing but does nothing.
  });

  app.run(root);
  std::cout.flush();
  return 0;
}