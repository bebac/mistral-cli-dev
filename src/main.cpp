#include "http/tls_connection.h"
#include "http/http_coordinator_task.h"
#include "api/types.h"
#include "agent/interactive_agent_task.h"
#include "agent/agent_system.h"
#include "agent/agent_system_task.h"
#include "agent/agent_system_stats_task.h"
#include "agent/agent_config.h"
#include "agent/workspace_bootstrap.h"
#include "tools/note_tools.h"
#include "template/template_engine.h"
#include "console/console_task.h"
#include "console/console_render_sink.h"
#include "slash_command/slash_command_executor_task.h"
#include "slash_command/slash_command_registry.h"
#include "trace/trace.h"
#include "common/signal_watcher_task.h"
#include "common/wakeup_pipe.h"

#include <dripcore/scheduler.h>
#include <dripcore/channel.h>
#include <uri.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <getopt.h>
#include <filesystem>
#include <signal.h>
#include <unistd.h>

// Global pipe write fd for SIGINT → dripcore event loop notification.
static int g_sigint_pipe_write_fd = -1;

static void sigint_handler(int)
{
  if (g_sigint_pipe_write_fd >= 0) {
    char buf = '1';
    ssize_t ret = ::write(g_sigint_pipe_write_fd, &buf, 1);
    (void)ret;
  }
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
  bool bootstrap_enabled         = true;
  bool bootstrap_update_managed  = false;
  std::string config_file_path;
  std::string tool_policy_file;
  std::string tool_default_policy = "deny";
  std::string agent_id            = "main";
  bool resume                     = false;

  // Parse command-line arguments.
  static struct option long_options[] = {
    {"config-file",        required_argument, 0, 'c'},
    {"notes-dir",          required_argument, 0, 'n'},
    {"host",               required_argument, 0, 'H'},
    {"port",               required_argument, 0, 'p'},
    {"agent",              required_argument, 0, 'a'},
    {"resume",             no_argument,       0, 'r'},
    {"system-prompt-show", no_argument,       0, 'e'},
    {"no-bootstrap",       no_argument,       0, 'B'},
    {"update-managed",     no_argument,       0, 'U'},
    {"help",               no_argument,       0, 'h'},
    {0, 0, 0, 0}
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "c:n:H:p:a:erP:D:BUh", long_options, nullptr)) != -1)
  {
    switch (opt) {
      case 'c': config_file_path = optarg; break;
      case 'n': notes_dir = optarg; break;
      case 'H': host = optarg; break;
      case 'p': port = std::atoi(optarg); break;
      case 'a': agent_id = optarg; break;
      case 'r': resume = true; break;
      case 'e': system_prompt_show = true; break;
      case 'B': bootstrap_enabled = false; break;
      case 'U': bootstrap_update_managed = true; break;
      case 'h':
        std::cout << "Usage: mistral-cli [OPTIONS]\n"
                  << "  -c, --config-file FILE         Configuration file path (required)\n"
                  << "  -n, --notes-dir DIR            Notes directory (default: .mistral-cli/notes)\n"
                  << "  -H, --host HOST                API host (default: api.mistral.ai)\n"
                  << "  -p, --port PORT                API port (default: 443)\n"
                  << "  -a, --agent AGENT              Agent ID to use (default: main)\n"
                  << "  -r, --resume                   Resume the latest chat session\n"
                  << "                                 (.mistral-cli/chats/chat.latest.json)\n"
                  << "  -e, --system-prompt-show       Show system prompt, then exit\n"
                  << "  -B, --no-bootstrap             Do not bootstrap the workspace\n"
                  << "  -U, --update-managed           Force-update managed files (e.g. sys_patchset_instructions.md)\n"
                  << "  -h, --help                     Show this help\n";
        return 0;
      default:
        return 1;
    }
  }

  // If config file not provide use default.
  const bool custom_config = !config_file_path.empty();
  if (config_file_path.empty()) {
    config_file_path = std::filesystem::path(base_dir) / "mistral-cli.json";
  }

  // Bootstrap the workspace with the embedded default configuration files.
  // Skipped when a custom config file is used: the user manages their own
  // setup in that case.
  if (!custom_config && bootstrap_enabled) {
    mistral::agent::bootstrap_options bootstrap_opts;
    bootstrap_opts.force_update = bootstrap_update_managed;
    try {
      const auto boot = mistral::agent::bootstrap_workspace(base_dir, bootstrap_opts);
      if (!boot.created.empty()) {
        std::cout << "created default files in " << base_dir << ": ";
        for (const auto& name : boot.created) std::cout << name << " ";
        std::cout << std::endl;
      }
      for (const auto& note : boot.updated) {
        std::cout << "updated " << base_dir << "/" << note << std::endl;
      }
      for (const auto& warning : boot.warnings) {
        std::cerr << "warning: " << warning << std::endl;
      }
    } catch (const std::exception &e) {
      std::cerr << "workspace bootstrap failed: " << e.what() << std::endl;
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
  auto agent_response_ch = std::make_shared<dripcore::channel<std::string>>(32);

  // Create slash command channels
  auto slash_cmd_request_ch = std::make_shared<dripcore::channel<mistral::slash_command::slash_command_request>>(4);
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

  // Validate the tool policy early. A broken policy.json (e.g. invalid
  // JSON) would otherwise throw from the agent task constructor inside
  // sched.spawn() below, escaping main() and aborting the process. Fail
  // fast with a clean error message instead.
  try {
    (void)agent_system.resolve_tool_policy(agent_config);
  } catch (const std::exception &e) {
    std::cerr << "error: cannot load tool policy for agent '" << agent_id
              << "': " << e.what() << std::endl;
    return 1;
  }

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

  // Create self-pipe for SIGINT → dripcore event loop notification.
  mistral::common::wakeup_pipe sigint_pipe;
  g_sigint_pipe_write_fd = sigint_pipe.write_fd();
  agent_system.set_shutdown_pipe_fd(sigint_pipe.write_fd());

  // Install SIGINT handler.
  struct sigaction sa{};
  sa.sa_handler = sigint_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);

  // Create scheduler and spawn tasks.
  dripcore::scheduler sched;

  // Spawn agent_system_task to capture responses
  sched.spawn<mistral::agent::agent_system_task>(agent_response_ch);

  // Spawn agent_system_stats_task to capture statistics
  sched.spawn<mistral::agent::agent_system_stats_task>(agent_system.stats_ch());

  // Spawn slash command executor task
  sched.spawn<mistral::slash_command::slash_command_executor_task>(
      slash_cmd_request_ch, slash_cmd_response_ch, agent_system.slash_commands());

  // Spawn console task with slash command channels
  sched.spawn<mistral::console::console_task>(
      prompt_ch, input_ch, slash_cmd_request_ch, slash_cmd_response_ch,
      agent_system.slash_commands());

  sched.spawn<mistral::net::http_coordinator_task>(host, port, api_key, request_ch);
  auto sink = std::make_shared<mistral::agent::console_render_sink>();
  sched.spawn<mistral::agent::interactive_agent_task>(agent_id, input_ch, agent_response_ch, prompt_ch, sink, resume);
  sched.spawn<mistral::common::signal_watcher_task>(sigint_pipe.read_fd());
  sched.run();

  // Restore default SIGINT handler. wakeup_pipe destructor closes fds.
  signal(SIGINT, SIG_DFL);
  g_sigint_pipe_write_fd = -1;

  return 0;
}
