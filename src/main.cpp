#include "http/tls_connection.h"
#include "http/http_task.h"
#include "api/mistral_types.h"
#include "agent/agent_task.h"
#include "agent/tool_registry.h"
#include "tools/file_tools.h"
#include "console/console_task.h"

#include <dripcore/scheduler.h>
#include <dripcore/channel.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <getopt.h>

int main(int argc, char* argv[])
{

  // Default configuration.
  std::string model         = "mistral-medium-latest";
  std::string system_prompt = "You are a helpful AI assistant with access to file system tools.";
  std::string host          = "api.mistral.ai";
  int port                  = 443;

  // Parse command-line arguments.
  static struct option long_options[] = {
    {"model",         required_argument, 0, 'm'},
    {"system-prompt", required_argument, 0, 's'},
    {"host",          required_argument, 0, 'H'},
    {"port",          required_argument, 0, 'p'},
    {"help",          no_argument,       0, 'h'},
    {0, 0, 0, 0}
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "m:s:H:p:h", long_options, nullptr)) != -1)
  {
    switch (opt) {
      case 'm': model = optarg; break;
      case 's': system_prompt = optarg; break;
      case 'H': host = optarg; break;
      case 'p': port = std::atoi(optarg); break;
      case 'h':
        std::cout << "Usage: mistral-cli [OPTIONS]\n"
                  << "  -m, --model MODEL          Model name (default: mistral-medium-latest)\n"
                  << "  -s, --system-prompt TEXT    System prompt\n"
                  << "  -H, --host HOST            API host (default: api.mistral.ai)\n"
                  << "  -p, --port PORT            API port (default: 443)\n"
                  << "  -h, --help                 Show this help\n";
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

  // Initialize OpenSSL.
  mistral::http::init_openssl();

  // Register tools.
  mistral::tools::file_tools file_tools;
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
