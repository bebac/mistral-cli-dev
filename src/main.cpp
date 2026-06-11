#include "http/tls_connection.h"
#include "http/http_task.h"
#include "api/mistral_types.h"
#include "agent/agent_task.h"
#include "agent/tool_registry.h"
#include "tools/file_tools.h"
#include "tools/note_tools.h"
#include "template/template_engine.h"
#include "console/console_task.h"

#include <dripcore/scheduler.h>
#include <dripcore/channel.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <getopt.h>

int main(int argc, char* argv[])
{

  // Default configuration.
  std::string model              = "mistral-medium-latest";
  std::string system_prompt      = "You are a helpful AI assistant with access to file system and note tools.";
  std::string system_prompt_file;
  std::string base_dir           = ".mistral-cli";
  std::string notes_dir          = ".mistral-cli/notes";
  std::string host               = "api.mistral.ai";
  int port                       = 443;
  bool system_prompt_show        = false;

  // Parse command-line arguments.
  static struct option long_options[] = {
    {"model",              required_argument, 0, 'm'},
    {"system-prompt",      required_argument, 0, 's'},
    {"system-prompt-file", required_argument, 0, 'f'},
    {"notes-dir",          required_argument, 0, 'n'},
    {"host",               required_argument, 0, 'H'},
    {"port",               required_argument, 0, 'p'},
    {"system-prompt-show", no_argument,       0, 'e'},
    {"help",               no_argument,       0, 'h'},
    {0, 0, 0, 0}
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "m:s:f:n:H:p:eh", long_options, nullptr)) != -1)
  {
    switch (opt) {
      case 'm': model = optarg; break;
      case 's': system_prompt = optarg; break;
      case 'f': system_prompt_file = optarg; break;
      case 'n': notes_dir = optarg; break;
      case 'H': host = optarg; break;
      case 'p': port = std::atoi(optarg); break;
      case 'e': system_prompt_show = true; break;
      case 'h':
        std::cout << "Usage: mistral-cli [OPTIONS]\n"
                  << "  -m, --model MODEL              Model name (default: mistral-medium-latest)\n"
                  << "  -s, --system-prompt TEXT        System prompt\n"
                  << "  -f, --system-prompt-file FILE   Load system prompt from file (supports templates)\n"
                  << "  -n, --notes-dir DIR             Notes directory (default: .mistral-cli/notes)\n"
                  << "  -H, --host HOST                API host (default: api.mistral.ai)\n"
                  << "  -p, --port PORT                API port (default: 443)\n"
                  << "  -e, --system-prompt-show       Show system prompt, then exit\n"
                  << "  -h, --help                     Show this help\n";
        return 0;
      default:
        return 1;
    }
  }

  // Read API key from environment.
  const char* api_key_env = std::getenv("MISTRAL_API_KEY");
  if (!api_key_env) {
    std::cerr << "error: MISTRAL_API_KEY environment variable not set\n";
    return 1;
  }
  std::string api_key(api_key_env);

  // Load system prompt from file if specified.
  if (!system_prompt_file.empty()) {
    try {
      mistral::tmpl::template_engine tmpl;
      tmpl.set_include_base(base_dir);
      tmpl.set_variable("model", model);
      tmpl.set_variable("notes_dir", notes_dir);
      system_prompt = tmpl.process_file(system_prompt_file);
    } catch (const std::exception& e) {
      std::cerr << "error loading system prompt file: " << e.what() << "\n";
      return 1;
    }
  }

  if (system_prompt_show)
  {
    std::cout << system_prompt << std::endl;
    return 0;
  }

  // Initialize OpenSSL.
  mistral::http::init_openssl();

  // Register tools.
  mistral::tools::file_tools file_tools;
  mistral::tools::note_tools notes(notes_dir);
  mistral::agent::tool_registry tools;

  tools.register_tool(
    "ls",
    "List files and directories. Returns entries with type indicator (f=file, d=directory, l=symlink), "
    "size for files, and relative path.",
    &file_tools, &mistral::tools::file_tools::list_files,
    mistral::agent::arg("path", "Directory path to list"),
    mistral::agent::arg("recursive", "Whether to list subdirectories recursively", false),
    mistral::agent::arg("max_depth", "Maximum recursion depth (0 for unlimited)", 1)
  );

  tools.register_tool(
    "read",
    "Read file contents. Use line_from and line_to for specific ranges. "
    "Set both to 0 to read entire file. Lines are 1-indexed.",
    &file_tools, &mistral::tools::file_tools::read_file,
    mistral::agent::arg("path", "File path to read"),
    mistral::agent::arg("line_from", "First line to read (1-indexed, 0 = from beginning)", 0),
    mistral::agent::arg("line_to", "Last line to read (inclusive, 0 = until end)", 0)
  );

  tools.register_tool(
    "write",
    "Write content to a file. Creates the file and parent directories if needed. "
    "Overwrites existing content.",
    &file_tools, &mistral::tools::file_tools::write_file,
    mistral::agent::arg("path", "File path to write to"),
    mistral::agent::arg("content", "Content to write")
  );

  // Note tools.
  tools.register_tool(
    "notes_list",
    "List notes in the notes directory. Shows path, title, and tags for each note. "
    "Pass an empty string to list all notes, or a subfolder name to filter.",
    &notes, &mistral::tools::note_tools::list_notes,
    mistral::agent::arg("folder", "Subfolder to list (empty for all)", std::string(""))
  );

  tools.register_tool(
    "notes_read",
    "Read a note from the notes directory. Resolves {{> path}} include directives. "
    "Returns title, tags, and rendered content. The .md extension is optional.",
    &notes, &mistral::tools::note_tools::read_note,
    mistral::agent::arg("path", "Note path relative to notes directory")
  );

  tools.register_tool(
    "notes_write",
    "Create or update a note. Use YAML frontmatter for title and tags:\n"
    "---\ntitle: My Note\ntags: [tag1, tag2]\n---\n"
    "Use {{> path}} to include other notes (partials). Put reusable snippets in _partials/.",
    &notes, &mistral::tools::note_tools::write_note,
    mistral::agent::arg("path", "Note path relative to notes directory"),
    mistral::agent::arg("content", "Note content (with optional frontmatter)")
  );

  tools.register_tool(
    "notes_search",
    "Search notes by text content and/or tag. At least one must be provided. "
    "Returns matching notes with path, title, tags, and matching line context.",
    &notes, &mistral::tools::note_tools::search_notes,
    mistral::agent::arg("query", "Text to search for (case-insensitive)", std::string("")),
    mistral::agent::arg("tag", "Filter by tag (case-insensitive)", std::string(""))
  );

  // Create channels.
  auto prompt_ch   = std::make_shared<dripcore::channel<std::string>>(1);
  auto input_ch    = std::make_shared<dripcore::channel<std::string>>(1);
  auto request_ch  = std::make_shared<dripcore::channel<mistral::api::api_request>>(4);
  auto response_ch = std::make_shared<dripcore::channel<mistral::api::api_event>>(64);

  std::cout << "mistral-cli (" << model << ")\n"
            << "Type your message, Ctrl-D to exit.\n" << std::endl;

  // Create scheduler and spawn tasks.
  dripcore::scheduler sched;

  sched.spawn<mistral::console::console_task>(prompt_ch, input_ch);
  sched.spawn<mistral::net::http_task>(host, port, api_key, request_ch, response_ch);
  sched.spawn<mistral::agent::agent_task>(model, system_prompt, tools,
                                          prompt_ch, input_ch, request_ch, response_ch);

  sched.run();

  return 0;
}
