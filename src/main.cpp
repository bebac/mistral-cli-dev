#include "http/tls_connection.h"
#include "http/http_task.h"
#include "http/http_coordinator_task.h"
#include "api/mistral_types.h"
#include "agent/interactive_agent_task.h"
#include "agent/agent_system.h"
#include "agent/agent_system_task.h"
#include "agent/agent_system_stats_task.h"
#include "agent/agent_config.h"
#include "tools/note_tools.h"
#include "template/template_engine.h"
#include "console/console_task.h"
#include "console/slash_command_executor_task.h"
#include "console/slash_command_registry.h"
#include "trace/trace.h"

#include <dripcore/scheduler.h>
#include <dripcore/channel.h>
#include <uri.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <getopt.h>
#include <filesystem>

// Collect all unique tags from all notes and write to a file
std::string generate_note_tags_file(const std::string &notes_dir, const std::string &base_dir)
{
  namespace fs = std::filesystem;

  std::set<std::string> all_tags;
  fs::path notes_path(notes_dir);

  if (!fs::exists(notes_path))
  {
    return "";
  }

  // Recursively scan all .md files in notes directory
  for (const auto &entry : fs::recursive_directory_iterator(notes_path))
  {
    if (!entry.is_regular_file())
      continue;
    if (entry.path().extension() != ".md")
      continue;

    std::ifstream f(entry.path());
    if (!f)
      continue;

    std::ostringstream ss;
    ss << f.rdbuf();
    auto parsed = mistral::tools::parse_frontmatter(ss.str());

    for (const auto &tag : parsed.front.tags)
    {
      all_tags.insert(tag);
    }
  }

  // Write tags to file in .mistral-cli directory
  fs::path tags_file = fs::path(base_dir) / "note_tags.txt";
  std::ofstream out(tags_file);
  if (!out)
  {
    std::cerr << "warning: could not write note tags file: " << tags_file << std::endl;
    return "";
  }

  out << "Available note tags:\n";
  for (const auto &tag : all_tags)
  {
    out << "- " << tag << "\n";
  }

  return tags_file.string();
}

void terminate_handler()
{
  try
  {
    if (auto e = std::current_exception()) {
      std::rethrow_exception(e);
    }
  }
  catch (const std::exception &ex) {
    std::cerr << "\n[FATAL] Unhandled exception: " << ex.what() << std::endl;
  }
  catch (...) {
    std::cerr << "\n[FATAL] Unhandled unknown exception" << std::endl;
  }
  std::abort();
}

int main(int argc, char* argv[])
{
  std::set_terminate(terminate_handler);

  // Default configuration.
  std::string base_dir           = ".mistral-cli";
  std::string notes_dir          = ".mistral-cli/notes";
  std::string host               = "api.mistral.ai";
  int port                       = 443;
  bool system_prompt_show        = false;
  std::string config_file_path;
  std::string tool_policy_file;
  std::string tool_default_policy = "deny";
  std::string agent_id            = "main";

  // Parse command-line arguments.
  static struct option long_options[] = {
    {"config-file",        required_argument, 0, 'c'},
    {"notes-dir",          required_argument, 0, 'n'},
    {"host",               required_argument, 0, 'H'},
    {"port",               required_argument, 0, 'p'},
    {"agent",              required_argument, 0, 'a'},
    {"system-prompt-show", no_argument,       0, 'e'},
    {"help",               no_argument,       0, 'h'},
    {0, 0, 0, 0}
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "c:n:H:p:a:eP:D:h", long_options, nullptr)) != -1)
  {
    switch (opt) {
      case 'c': config_file_path = optarg; break;
      case 'n': notes_dir = optarg; break;
      case 'H': host = optarg; break;
      case 'p': port = std::atoi(optarg); break;
      case 'a': agent_id = optarg; break;
      case 'e': system_prompt_show = true; break;
      case 'h':
        std::cout << "Usage: mistral-cli [OPTIONS]\n"
                  << "  -c, --config-file FILE         Configuration file path (required)\n"
                  << "  -n, --notes-dir DIR            Notes directory (default: .mistral-cli/notes)\n"
                  << "  -H, --host HOST                API host (default: api.mistral.ai)\n"
                  << "  -p, --port PORT                API port (default: 443)\n"
                  << "  -a, --agent AGENT              Agent ID to use (default: main)\n"
                  << "  -e, --system-prompt-show       Show system prompt, then exit\n"
                  << "  -h, --help                     Show this help\n";
        return 0;
      default:
        return 1;
    }
  }

  // If config file not provide use default.
  if (config_file_path.empty()) {
    config_file_path = std::filesystem::path(base_dir) / "mistral-cli.json";
  }

  auto log_filename = std::filesystem::path(base_dir) / "trace.log";
  mistral::trace::logger::instance().set_log_path(log_filename.string());
  mistral::trace::logger::instance().enable(false);

  std::string api_key;

  // Initialize OpenSSL.
  mistral::net::init_openssl();

  // Create channels.
  auto prompt_ch   = std::make_shared<dripcore::channel<std::string>>(1);
  auto input_ch    = std::make_shared<dripcore::channel<std::string>>(1);
  auto request_ch  = std::make_shared<dripcore::channel<mistral::api::api_request>>(4);
  auto agent_response_ch = std::make_shared<dripcore::channel<std::string>>(32);

  // Create slash command channels
  auto slash_cmd_request_ch = std::make_shared<dripcore::channel<mistral::console::slash_command_request>>(4);
  auto slash_cmd_response_ch = std::make_shared<dripcore::channel<std::string>>(4);

  // Initialize agent system with config file
  auto& agent_system = mistral::agent::agent_system::instance();
  std::filesystem::path config_file(config_file_path);

  if (!std::filesystem::exists(config_file))
  {
    std::cerr << "configuration file '" << config_file << "' not found" << std::endl;
    return 1;
  }

  std::cout << "Loading configuration from: " << config_file << std::endl;
  try
  {
    agent_system.initialize(config_file.string(), base_dir, notes_dir, request_ch);
  }
  catch (const std::exception &e)
  {
    std::cerr << "error loading configuration file: " << e.what() << std::endl;
    return 1;
  }

  // Validate agent exists in configuration
  if (!agent_system.has_agent_config(agent_id)) {
    std::cerr << "error: agent '" << agent_id << "' not found in configuration" << std::endl;
    std::cerr << "Available agents: ";
    for (const auto& [name, _] : agent_system.get_config().agents) {
      std::cerr << name << " ";
    }
    std::cerr << std::endl;
    return 1;
  }

  // Get agent configuration
  const auto& agent_config = agent_system.get_agent_config(agent_id);
  const auto& model_config = agent_system.get_model_config(agent_config.model);

  // Use base_url from config if command-line host wasn't overridden
  if (host == "api.mistral.ai" && !model_config.base_url.empty())
  {
    auto parse_result = uri::parse(model_config.base_url);
    if (std::holds_alternative<uri::uri_view>(parse_result))
    {
      const auto& parsed_uri = std::get<uri::uri_view>(parse_result);

      auto config_host = parsed_uri.host();
      auto config_port = 443; // Default to HTTPS port

      // Extract port if present
      auto port_str = parsed_uri.port();
      if (!port_str.empty())
      {
        try {
          config_port = std::stoi(std::string(port_str));
        }
        catch (...) {
          // If port parsing fails, keep default
        }
      }

      host = config_host;

      if (port == 443)
      { // Only use config port if command-line didn't override
        port = config_port;
      }
    }
  }

  // Read API key from environment variable specified in model config
  const char* api_key_env = std::getenv(model_config.api_key_env.c_str());
  if (!api_key_env) {
    std::cerr << "error: " << model_config.api_key_env << " environment variable not set\n";
    return 1;
  }
  api_key = std::string(api_key_env);

  // Resolve model ID and system prompt
  std::string resolved_model = model_config.model_id;
  std::string resolved_system_prompt = agent_system.resolve_system_prompt(
      agent_config, resolved_model);

  // Show system prompt and exit if requested
  if (system_prompt_show) {
    std::cout << resolved_system_prompt << std::endl;
    return 0;
  }

  // Create filtered tool registry based on agent's allowed_tools
  auto tools = agent_system.create_tool_registry(agent_config.available_tools);

  std::cout << "mistral-cli (" << resolved_model << ")\n"
            << "Type your message, Ctrl-D to exit.\n" << std::endl;

  // Create scheduler and spawn tasks.
  dripcore::scheduler sched;

  // Spawn agent_system_task to capture responses
  sched.spawn<mistral::agent::agent_system_task>(agent_response_ch);

  // Spawn agent_system_stats_task to capture statistics
  sched.spawn<mistral::agent::agent_system_stats_task>(agent_system.stats_ch());

  // Spawn slash command executor task
  sched.spawn<mistral::console::slash_command_executor_task>(
      slash_cmd_request_ch, slash_cmd_response_ch);

  // Spawn console task with slash command channels
  sched.spawn<mistral::console::console_task>(
      prompt_ch, input_ch, slash_cmd_request_ch, slash_cmd_response_ch);

  //sched.spawn<mistral::net::http_task>(host, port, api_key, request_ch);
  sched.spawn<mistral::net::http_coordinator_task>(host, port, api_key, request_ch);
  sched.spawn<mistral::agent::interactive_agent_task>(agent_id, input_ch, agent_response_ch, prompt_ch);
  sched.run();

  return 0;
}
