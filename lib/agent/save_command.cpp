#include "agent/save_command.h"
#include "agent/agent_system.h"

#include <filesystem>
#include <fstream>

namespace mistral::agent
{
  std::string save_command::name() const
  {
    return "save";
  }

  std::string save_command::description() const
  {
    return "Save the last AI response to a file";
  }

  std::string save_command::usage() const
  {
    return "[filename]";
  }

  console::slash_command::execution_context save_command::context() const
  {
    return execution_context::AGENT;
  }

  std::string save_command::execute_agent(
    const std::string &args,
    agent::agent_system &agent_sys)
  {
    // Get the last response from agent_system
    std::string response = agent_sys.get_last_response();

    if (response.empty())
    {
      return "Error: No AI response to save. Have you sent a message yet?";
    }

    // Determine filename
    std::string filename = args.empty() ? "last_response.txt" : args;

    // Create parent directories if needed
    std::filesystem::path filepath(filename);
    if (filepath.has_parent_path())
    {
      std::filesystem::create_directories(filepath.parent_path());
    }

    // Write to file
    std::ofstream out(filename, std::ios::binary);
    if (!out)
    {
      return "Error: Could not open file for writing: " + filename;
    }

    out << response;

    if (!out)
    {
      return "Error: Failed to write to file: " + filename;
    }

    return "Saved last response to: " + filename;
  }
} // namespace mistral::console
