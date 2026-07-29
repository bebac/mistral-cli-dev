#ifndef MISTRAL_SUBAGENT_TASK_H
#define MISTRAL_SUBAGENT_TASK_H

#include "agent/agent_task_base.h"
#include "agent/policy_engine.h"
#include "agent/approval_manager.h"
#include "api/mistral_api.h"

#include <dripcore/task_handle.h>
#include <dripcore/awaitable.h>

namespace mistral::agent
{

  class subagent_task : public agent_task_base
  {
  public:
    using agent_task_base::agent_task_base;

    dripcore::task_handle execute();

  protected:
    void thinking_begin() override;
    void thinking_end() override;
    void agent_loop_on_sending_chat_request() override;
    void agent_loop_on_message_content(const api::message_content& mc) override;
    auto agent_loop_do_tool_call(const api::tool_call& tc) -> dripcore::awaitable<void> override;
    void agent_loop_on_error(const std::string& error) override;
  };

} // namespace mistral::agent

#endif // MISTRAL_SUBAGENT_TASK_H