#include "agent/model_command.h"

#include "agent/agent_system.h"

#include <dripcore/channel.h>

#include <algorithm>
#include <string>
#include <utility>

namespace mistral::agent
{
  std::string model_command::name() const
  {
    return "model";
  }

  std::string model_command::description() const
  {
    return "Show or switch the active model";
  }

  slash_command::command_spec model_command::spec() const
  {
    using namespace mistral::slash_command;

    arg_spec model_id;
    model_id.name = "model_id";
    model_id.description = "model id to switch to";
    model_id.source = candidate_source::model_ids;
    // The candidates are the point of the command: "/model " + Enter takes
    // the highlighted id, the way "/mcp connect <server>" does. The bare
    // "/model" (no space) still runs and lists the ids, and the popup marks
    // the active one.
    model_id.prefer_candidate = true;

    command_spec out;
    out.name = name();
    out.summary = description();
    out.args.push_back(std::move(model_id));
    return out;
  }

  dripcore::awaitable<std::string> model_command::execute(
    dripcore::task&,
    const std::string& args,
    agent::agent_system& agent_sys
  )
  {
    const std::string agent_id = agent_sys.current_agent_id().value_or("main");
    if (!agent_sys.has_agent_config(agent_id))
    {
      co_return "Error: agent '" + agent_id + "' not found in configuration";
    }

    const auto& agent_cfg = agent_sys.get_agent_config(agent_id);
    const std::vector<std::string> ids = agent_sys.model_ids_for(agent_cfg.model);
    if (ids.empty())
    {
      co_return "Error: model entry '" + agent_cfg.model + "' not found in configuration";
    }

    // Trim the argument.
    std::string model_id = args;
    model_id.erase(0, model_id.find_first_not_of(" \t"));
    model_id.erase(model_id.find_last_not_of(" \t") + 1);

    // Without arguments: list the selectable ids, marking the active one.
    if (model_id.empty())
    {
      const std::string active = agent_sys.resolve_model_id(agent_cfg.model);
      std::string result = "model entry '" + agent_cfg.model + "':\n";
      for (const auto& id : ids)
      {
        result += id == active ? "  * " : "    ";
        result += id + "\n";
      }
      result += "use /model <model_id> to switch";
      co_return result;
    }

    if (std::find(ids.begin(), ids.end(), model_id) == ids.end())
    {
      std::string valid;
      for (const auto& id : ids)
      {
        valid += (valid.empty() ? "" : ", ") + id;
      }
      co_return "Error: '" + model_id + "' is not offered by model entry '" + agent_cfg.model +
                "' (available: " + valid + ")";
    }

    if (model_id == agent_sys.resolve_model_id(agent_cfg.model))
    {
      co_return "Already using " + model_id;
    }

    // Route the switch to the interactive agent task, which owns the
    // conversation and the request machinery.
    if (auto input_ch = agent_sys.get_current_input_ch())
    {
      if (!input_ch->try_send(std::string(model_request_prefix) + model_id))
      {
        co_return "Error: agent input is busy, try again later";
      }
      co_return "Switching model to " + model_id + "...";
    }

    // No interactive task (e.g. piped mode): record the selection so
    // tasks created afterwards pick it up.
    try
    {
      agent_sys.set_active_model_id(agent_cfg.model, model_id);
    }
    catch (const std::exception& e)
    {
      co_return std::string("Error: ") + e.what();
    }
    co_return "Model switched to " + model_id;
  }
} // namespace mistral::agent