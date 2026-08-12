#include "agent/load_command.h"
#include "agent/agent_system.h"

#include <filesystem>
#include <fstream>
#include <dripcore/task.h>

namespace mistral::agent
{
  std::string load_command::name() const
  {
    return "load";
  }

  std::string load_command::description() const
  {
    return "Load a file and set it as the last response";
  }

  std::string load_command::usage() const
  {
    return "[filename]";
  }

  dripcore::awaitable<std::string> load_command::execute(
    dripcore::task&,
    const std::string &args,
    agent::agent_system &agent_sys)
  {
    // Determine filename
    std::string filename = args.empty() ? "last_response.txt" : args;

    // Check if file exists
    if (!std::filesystem::exists(filename)) {
      co_return "Error: File not found: " + filename;
    }

    // Read file
    std::ifstream in(filename, std::ios::binary);
    if (!in) {
      co_return "Error: Could not open file for reading: " + filename;
    }

    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());

    if (content.empty()) {
      co_return "Error: File is empty: " + filename;
    }

    // Set the content as the last response
    agent_sys.set_last_response(content);

    co_return "Loaded file: " + filename;
  }
} // namespace mistral::agent