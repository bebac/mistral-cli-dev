#ifndef MISTRAL_AGENT_SYSTEM_STATS_TASK_H
#define MISTRAL_AGENT_SYSTEM_STATS_TASK_H

#include <dripcore/task.h>
#include <dripcore/channel.h>
#include <dripcore/task_handle.h>
#include <dripcore/awaitable.h>

#include <nlohmann/json.hpp>

#include <memory>

namespace mistral::agent
{
  using json = nlohmann::ordered_json;

  // Statistics message types
  enum class stat_id
  {
    api_usage
  };

  // Message for statistics updates
  struct stat_message
  {
    stat_id id;
    json data;
  };

  /**
   * @brief Task that receives and processes statistics messages.
   * Handles different stat types (api_usage, etc.) and updates agent_system.
   */
  class agent_system_stats_task : public dripcore::task
  {
  public:
    explicit agent_system_stats_task(
      std::shared_ptr<dripcore::channel<stat_message>> stats_ch);

    dripcore::awaitable<> body() override;

  private:
    std::shared_ptr<dripcore::channel<stat_message>> stats_ch_;
  };
} // namespace mistral::agent

#endif // MISTRAL_AGENT_SYSTEM_STATS_TASK_H
