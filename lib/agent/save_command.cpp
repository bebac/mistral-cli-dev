#include "agent/save_command.h"
#include "agent/agent_system.h"

#include <filesystem>
#include <fstream>
#include <dripcore/task.h>

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

  dripcore::awaitable<std::string> save_command::execute(
    dripcore::task&,
    const std::string &args,
    agent::agent_system &agent_sys)
  {
    // Get the last response from agent_system
    std::string response = agent_sys.get_last_response();

    if (response.empty())
    {
      co_return "Error: No AI response to save. Have you sent a message yet?";
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
      co_return "Error: Could not open file for writing: " + filename;
    }

    out << response;

    if (!out)
    {
      co_return "Error: Failed to write to file: " + filename;
    }

    co_return "Saved last response to: " + filename;
  }
} // namespace mistral::console
