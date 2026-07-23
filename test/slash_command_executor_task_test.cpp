// DOCTEST unit tests for slash_command_executor_task
#include <doctest/doctest.h>

#include "console/slash_command_executor_task.h"
#include "console/slash_command_registry.h"
#include "agent/agent_system.h"

#include <dripcore/channel.h>
#include <dripcore/scheduler.h>
#include <dripcore/task.h>
#include <dripcore/task_handle.h>

#include <string>
#include <thread>
#include <memory>

namespace
{

  // Test command for AGENT context - defined in anonymous namespace to avoid
  // symbol collision with slash_command_registry_test.cpp
  class executor_test_agent_command : public mistral::console::slash_command
  {
  public:
    std::string name() const override { return "test_cmd"; }
    std::string description() const override { return "Test command"; }
    mistral::console::slash_command::execution_context context() const override { return mistral::console::slash_command::execution_context::AGENT; }

    std::string execute_agent(const std::string &args, mistral::agent::agent_system &) override
    {
      return "Response: " + args;
    }
  };

  // Test command for IMMEDIATE context (should fail in executor)
  class executor_test_immediate_command : public mistral::console::slash_command
  {
  public:
    std::string name() const override { return "immediate_cmd"; }
    std::string description() const override { return "Immediate command"; }
    mistral::console::slash_command::execution_context context() const override { return mistral::console::slash_command::execution_context::IMMEDIATE; }

    std::string execute_immediate(const std::string &args) override
    {
      return "Immediate: " + args;
    }
  };

  // Fixture for slash_command_executor_task tests
  struct SlashCommandExecutorTestFixture
  {
    // A dripcore task that sends a request and collects the response
    class response_collector_task : public dripcore::task
    {
    public:
      response_collector_task(
          std::shared_ptr<dripcore::channel<mistral::console::slash_command_request>> request_ch,
          std::shared_ptr<dripcore::channel<std::string>> response_ch,
          mistral::console::slash_command_request req,
          std::string &out_response)
          : request_ch_(std::move(request_ch)), response_ch_(std::move(response_ch)), req_(std::move(req)), out_response_(out_response)
      {
      }

      dripcore::task_handle execute()
      {
        // Send the request
        request_ch_->try_send(std::move(req_));
        request_ch_->close();

        // Receive the response
        auto response = co_await response_ch_->recv();
        if (response)
        {
          out_response_ = std::move(*response);
        }
        else
        {
          out_response_ = "";
        }

        co_return;
      }

    private:
      std::shared_ptr<dripcore::channel<mistral::console::slash_command_request>> request_ch_;
      std::shared_ptr<dripcore::channel<std::string>> response_ch_;
      mistral::console::slash_command_request req_;
      std::string &out_response_;
    };

    std::string execute_and_collect_response(
        std::shared_ptr<dripcore::channel<mistral::console::slash_command_request>> request_ch,
        std::shared_ptr<dripcore::channel<std::string>> response_ch,
        mistral::console::slash_command_request req)
    {
      std::string response;
      dripcore::scheduler scheduler;

      scheduler.spawn<mistral::console::slash_command_executor_task>(
          request_ch, response_ch);
      scheduler.spawn<response_collector_task>(
          request_ch, response_ch, std::move(req), response);

      scheduler.run();
      return response;
    }
  };

} // anonymous namespace

TEST_CASE("slash_command_executor_task: execute AGENT command")
{
  auto request_ch = std::make_shared<dripcore::channel<mistral::console::slash_command_request>>(1);
  auto response_ch = std::make_shared<dripcore::channel<std::string>>(1);

  auto &registry = mistral::console::slash_command_registry::instance();
  registry.register_command(std::make_unique<executor_test_agent_command>());

  SlashCommandExecutorTestFixture fixture;
  mistral::console::slash_command_request req;
  req.command_name = "test_cmd";
  req.args = "hello";

  std::string response = fixture.execute_and_collect_response(
      request_ch, response_ch, std::move(req));

  CHECK(response == "Response: hello");
}

TEST_CASE("slash_command_executor_task: unknown command")
{
  auto request_ch = std::make_shared<dripcore::channel<mistral::console::slash_command_request>>(1);
  auto response_ch = std::make_shared<dripcore::channel<std::string>>(1);

  SlashCommandExecutorTestFixture fixture;
  mistral::console::slash_command_request req;
  req.command_name = "unknown_cmd";
  req.args = "";

  std::string response = fixture.execute_and_collect_response(
      request_ch, response_ch, std::move(req));

  CHECK(response.find("Unknown command: /unknown_cmd") != std::string::npos);
}

TEST_CASE("slash_command_executor_task: IMMEDIATE command in AGENT executor")
{
  auto request_ch = std::make_shared<dripcore::channel<mistral::console::slash_command_request>>(1);
  auto response_ch = std::make_shared<dripcore::channel<std::string>>(1);

  auto &registry = mistral::console::slash_command_registry::instance();
  registry.register_command(std::make_unique<executor_test_immediate_command>());

  SlashCommandExecutorTestFixture fixture;
  mistral::console::slash_command_request req;
  req.command_name = "immediate_cmd";
  req.args = "";

  std::string response = fixture.execute_and_collect_response(
      request_ch, response_ch, std::move(req));

  CHECK(response.find("must be executed in IMMEDIATE context") != std::string::npos);
}

TEST_CASE("slash_command_executor_task: exception handling")
{
  auto request_ch = std::make_shared<dripcore::channel<mistral::console::slash_command_request>>(1);
  auto response_ch = std::make_shared<dripcore::channel<std::string>>(1);

  class throwing_command : public mistral::console::slash_command
  {
  public:
    std::string name() const override { return "throw_cmd"; }
    std::string description() const override { return "Throws"; }
    mistral::console::slash_command::execution_context context() const override { return mistral::console::slash_command::execution_context::AGENT; }
    std::string execute_agent(const std::string &, mistral::agent::agent_system &) override
    {
      throw std::runtime_error("Test error");
    }
  };

  auto &registry = mistral::console::slash_command_registry::instance();
  registry.register_command(std::make_unique<throwing_command>());

  SlashCommandExecutorTestFixture fixture;
  mistral::console::slash_command_request req;
  req.command_name = "throw_cmd";
  req.args = "";

  std::string response = fixture.execute_and_collect_response(
      request_ch, response_ch, std::move(req));

  CHECK(response.find("Error executing /throw_cmd: Test error") != std::string::npos);
}

TEST_CASE("slash_command_executor_task: empty response")
{
  auto request_ch = std::make_shared<dripcore::channel<mistral::console::slash_command_request>>(1);
  auto response_ch = std::make_shared<dripcore::channel<std::string>>(1);

  class empty_command : public mistral::console::slash_command
  {
  public:
    std::string name() const override { return "empty_cmd"; }
    std::string description() const override { return "Empty"; }
    mistral::console::slash_command::execution_context context() const override { return mistral::console::slash_command::execution_context::AGENT; }
    std::string execute_agent(const std::string &, mistral::agent::agent_system &) override
    {
      return "";
    }
  };

  auto &registry = mistral::console::slash_command_registry::instance();
  registry.register_command(std::make_unique<empty_command>());

  SlashCommandExecutorTestFixture fixture;
  mistral::console::slash_command_request req;
  req.command_name = "empty_cmd";
  req.args = "";

  std::string response = fixture.execute_and_collect_response(
      request_ch, response_ch, std::move(req));

  CHECK(response.empty());
}
