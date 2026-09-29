#include "tui/tui_app.h"
#include "http/tls_connection.h"
#include "api/types.h"
#include "agent/agent_system.h"
#include "agent/agent_config.h"
#include "agent/workspace_bootstrap.h"
#include "trace/trace.h"

#include <dripcore/channel.h>
#include <uri.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char* argv[])
{
  std::string base_dir    = ".mistral-cli";
  std::string notes_dir   = ".mistral-cli/notes";
  std::string host        = "api.mistral.ai";
  int port                = 443;
  std::string agent_id    = "main";
  std::string config_file_path;
  std::string theme_name  = "dark";
  bool bootstrap_enabled = true;
  bool bootstrap_update_managed = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-a" && i + 1 < argc) { agent_id = argv[++i]; }
    else if (arg == "-H" && i + 1 < argc) { host = argv[++i]; }
    else if (arg == "-p" && i + 1 < argc) { port = std::atoi(argv[++i]); }
    else if (arg == "-c" && i + 1 < argc) { config_file_path = argv[++i]; }
    else if (arg == "--theme" && i + 1 < argc) { theme_name = argv[++i]; }
    else if (arg == "--no-bootstrap") { bootstrap_enabled = false; }
    else if (arg == "--update-managed") { bootstrap_update_managed = true; }
    else if (arg == "-h") {
      std::cout << "Usage: mistral-tui [OPTIONS]\n"
                << "  -c FILE       Config file\n"
                << "  -H HOST       API host\n"
                << "  -p PORT       API port\n"
                << "  -a ID         Agent ID\n"
                << "  --theme NAME  Theme: dark, light, nord,"
                << " tokyo-night, solarized-light\n"
                << "  --no-bootstrap       Do not bootstrap the workspace\n"
                << "  --update-managed     Force-update managed files"
                << " (e.g. sys_patchset_instructions.md)\n";
      return 0;
    }
  }

  const bool custom_config = !config_file_path.empty();
  if (config_file_path.empty()) {
    config_file_path =
      std::filesystem::path(base_dir) / "mistral-cli.json";
  }

  // Bootstrap the workspace with the embedded default configuration files.
  // Skipped when a custom config file is used. Notices go to stderr: the TUI
  // takes over stdout.
  if (!custom_config && bootstrap_enabled) {
    mistral::agent::bootstrap_options bootstrap_opts;
    bootstrap_opts.force_update = bootstrap_update_managed;
    try {
      const auto boot = mistral::agent::bootstrap_workspace(base_dir, bootstrap_opts);
      if (!boot.created.empty()) {
        std::cerr << "created default files in " << base_dir << ": ";
        for (const auto& name : boot.created) std::cerr << name << " ";
        std::cerr << std::endl;
      }
      for (const auto& note : boot.updated) {
        std::cerr << "updated " << base_dir << "/" << note << std::endl;
      }
      for (const auto& warning : boot.warnings) {
        std::cerr << "warning: " << warning << std::endl;
      }
    } catch (const std::exception& e) {
      std::cerr << "workspace bootstrap failed: " << e.what() << std::endl;
    }
  }

  auto log_filename = std::filesystem::path(base_dir) / "trace.log";
  mistral::trace::logger::instance().set_log_path(log_filename.string());
  mistral::trace::logger::instance().enable(false);

  mistral::net::init_openssl();

  auto prompt_ch = std::make_shared<dripcore::channel<std::string>>(4);
  auto input_ch  = std::make_shared<dripcore::channel<std::string>>(1);
  auto request_ch =
    std::make_shared<dripcore::channel<mistral::api::api_request>>(4);
  auto agent_response_ch =
    std::make_shared<dripcore::channel<std::string>>(32);
  auto cmd_request_ch =
    std::make_shared<dripcore::channel<
      mistral::slash_command::slash_command_request>>(4);
  auto slash_cmd_response_ch =
    std::make_shared<dripcore::channel<std::string>>(4);

  auto& agent_system = mistral::agent::agent_system::instance();
  std::filesystem::path config_file(config_file_path);

  if (!std::filesystem::exists(config_file)) {
    std::cerr << "configuration file '" << config_file
              << "' not found" << std::endl;
    return 1;
  }

  try {
    agent_system.initialize(
      config_file.string(), base_dir, notes_dir, request_ch);
  } catch (const std::exception& e) {
    std::cerr << "error loading configuration: " << e.what() << std::endl;
    return 1;
  }

  if (!agent_system.has_agent_config(agent_id)) {
    std::cerr << "error: agent '" << agent_id << "' not found" << std::endl;
    return 1;
  }

  const auto& agent_config = agent_system.get_agent_config(agent_id);
  const auto& model_config =
    agent_system.get_model_config(agent_config.model);

  if (host == "api.mistral.ai" && !model_config.base_url.empty()) {
    auto parse_result = uri::parse(model_config.base_url);
    if (std::holds_alternative<uri::uri_view>(parse_result)) {
      const auto& parsed_uri = std::get<uri::uri_view>(parse_result);
      host = parsed_uri.host();
      auto port_str = parsed_uri.port();
      if (!port_str.empty()) {
        try { port = std::stoi(std::string(port_str)); } catch (...) {}
      }
    }
  }

  const char* api_key_env =
    std::getenv(model_config.api_key_env.c_str());
  if (!api_key_env) {
    std::cerr << "error: " << model_config.api_key_env
              << " not set" << std::endl;
    return 1;
  }
  std::string api_key(api_key_env);

  std::string resolved_model = model_config.model_id;

  mistral::tui::tui_config tui_cfg = {
    .agent_id            = agent_id,
    .host                = host,
    .port                = port,
    .api_key             = api_key,
    .theme_name          = theme_name,
    .resolved_model      = resolved_model,
    .prompt_ch           = prompt_ch,
    .input_ch            = input_ch,
    .request_ch          = request_ch,
    .agent_response_ch   = agent_response_ch,
    .cmd_request_ch      = cmd_request_ch,
    .slash_cmd_response_ch = slash_cmd_response_ch
  };

  try {
    mistral::tui::tui_app app(std::move(tui_cfg));
    app.run();
    app.shutdown();
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}