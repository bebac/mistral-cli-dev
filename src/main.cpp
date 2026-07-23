#include "http/tls_connection.h"
#include "http/http_task.h"
#include "api/mistral_types.h"
#include "agent/interactive_agent_task.h"
#include "agent/agent_system.h"
#include "agent/agent_config.h"
#include "tools/note_tools.h"
#include "template/template_engine.h"
#include "console/console_task.h"
#include "console/slash_command_executor_task.h"
#include "console/slash_command_registry.h"
#include "trace/trace.h"

#include <dripcore/scheduler.h>
#include <dripcore/channel.h>

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
  std::string model              = "mistral-medium-latest";
  std::string base_dir           = ".mistral-cli";
  std::string notes_dir          = ".mistral-cli/notes";
  std::string host               = "api.mistral.ai";
  int port                       = 443;
  bool system_prompt_show        = false;
  std::string tool_policy_file;
  std::string tool_default_policy = "deny";

  // Parse command-line arguments.
  static struct option long_options[] = {
    {"model",              required_argument, 0, 'm'},
    {"notes-dir",          required_argument, 0, 'n'},
    {"host",               required_argument, 0, 'H'},
    {"port",               required_argument, 0, 'p'},
    {"system-prompt-show", no_argument,       0, 'e'},
    {"help",               no_argument,       0, 'h'},
    {0, 0, 0, 0}
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "m:n:H:p:eP:D:h", long_options, nullptr)) != -1)
  {
    switch (opt) {
      case 'm': model = optarg; break;
      case 'n': notes_dir = optarg; break;
      case 'H': host = optarg; break;
      case 'p': port = std::atoi(optarg); break;
      case 'e': system_prompt_show = true; break;
      case 'h':
        std::cout << "Usage: mistral-cli [OPTIONS]\n"
                  << "  -m, --model MODEL              Model name (default: mistral-medium-latest)\n"
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
  auto response_ch = std::make_shared<dripcore::channel<mistral::api::api_event>>(64);

  // Create slash command channels
  auto slash_cmd_request_ch = std::make_shared<dripcore::channel<mistral::console::slash_command_request>>(4);
  auto slash_cmd_response_ch = std::make_shared<dripcore::channel<std::string>>(4);

  // Initialize agent system with config file or command-line arguments
  auto config_file = std::filesystem::path(base_dir) / "mistral-cli.json";
  auto& agent_system = mistral::agent::agent_system::instance();

  if (std::filesystem::exists(config_file))
  {
    std::cout << "Loading configuration from: " << config_file << std::endl;
    try
    {
      agent_system.initialize(config_file.string(), base_dir, notes_dir, request_ch, response_ch);
    }
    catch (const std::exception &e)
    {
      std::cerr << "error loading configuration file: " << e.what() << std::endl;
      return 1;
    }
  }
  else
  {
    std::cout << "No config file found at " << config_file
              << ", using command-line arguments" << std::endl;
    agent_system.initialize_from_arguments(model, base_dir, notes_dir, request_ch, response_ch);
  }

  // Get agent configuration
  const auto& agent_config = agent_system.get_agent_config("main");
  const auto& model_config = agent_system.get_model_config(agent_config.model);

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

  std::cout << "mistral-cli (" << model << ")\n"
            << "Type your message, Ctrl-D to exit.\n" << std::endl;

  // Create scheduler and spawn tasks.
  dripcore::scheduler sched;

  // Spawn slash command executor task
  sched.spawn<mistral::console::slash_command_executor_task>(
      slash_cmd_request_ch, slash_cmd_response_ch);

  // Spawn console task with slash command channels
  sched.spawn<mistral::console::console_task>(
      prompt_ch, input_ch, slash_cmd_request_ch, slash_cmd_response_ch);

  sched.spawn<mistral::net::http_task>(host, port, api_key, request_ch, response_ch);
  sched.spawn<mistral::agent::interactive_agent_task>("main", input_ch, nullptr, prompt_ch);
  sched.run();

  return 0;
}
