// DOCTEST unit tests for slash_command_executor_task
#include <doctest/doctest.h>

#include "console/slash_command_executor_task.h"
#include "console/slash_command_registry.h"
#include "agent/agent_system.h"

#include <dripcore/channel.h>
#include <dripcore/scheduler.h>
#include <dripcore/task.h>
#include <dripcore/task_handle.h>
#include <dripcore/awaitable.h>

#include <string>
#include <thread>
#include <memory>

namespace
{

  // Test command for executor task
  class executor_test_agent_command : public mistral::console::slash_command
  {
  public:
    std::string name() const override { return "test_cmd"; }
    std::string description() const override { return "Test command"; }

    dripcore::awaitable<std::string> execute(
        dripcore::task&,
        const std::string &args,
        mistral::agent::agent_system &) override
    {
      co_return "Response: " + args;
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
        auto response = co_await response_ch_->recv(this);
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
        std::shared_ptr<mistral::console::slash_command_registry> registry,
        mistral::console::slash_command_request req)
    {
      std::string response;
      dripcore::scheduler scheduler;

      scheduler.spawn<mistral::console::slash_command_executor_task>(
          request_ch, response_ch, registry);
      scheduler.spawn<response_collector_task>(
          request_ch, response_ch, std::move(req), response);

      scheduler.run();
      return response;
    }
  };

} // anonymous namespace

TEST_CASE("slash_command_executor_task: execute command")
{
  auto request_ch = std::make_shared<dripcore::channel<mistral::console::slash_command_request>>(1);
  auto response_ch = std::make_shared<dripcore::channel<std::string>>(1);

  auto registry = std::make_shared<mistral::console::slash_command_registry>();
  registry->register_command(std::make_unique<executor_test_agent_command>());

  SlashCommandExecutorTestFixture fixture;
  mistral::console::slash_command_request req;
  req.command_name = "test_cmd";
  req.args = "hello";

  std::string response = fixture.execute_and_collect_response(
      request_ch, response_ch, registry, std::move(req));

  CHECK(response == "Response: hello");
}

TEST_CASE("slash_command_executor_task: unknown command")
{
  auto request_ch = std::make_shared<dripcore::channel<mistral::console::slash_command_request>>(1);
  auto response_ch = std::make_shared<dripcore::channel<std::string>>(1);

  auto registry = std::make_shared<mistral::console::slash_command_registry>();

  SlashCommandExecutorTestFixture fixture;
  mistral::console::slash_command_request req;
  req.command_name = "unknown_cmd";
  req.args = "";

  std::string response = fixture.execute_and_collect_response(
      request_ch, response_ch, registry, std::move(req));

  CHECK(response.find("Unknown command: /unknown_cmd") != std::string::npos);
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

    dripcore::awaitable<std::string> execute(
        dripcore::task&,
        const std::string &,
        mistral::agent::agent_system &) override
    {
      throw std::runtime_error("Test error");
    }
  };

  auto registry = std::make_shared<mistral::console::slash_command_registry>();
  registry->register_command(std::make_unique<throwing_command>());

  SlashCommandExecutorTestFixture fixture;
  mistral::console::slash_command_request req;
  req.command_name = "throw_cmd";
  req.args = "";

  std::string response = fixture.execute_and_collect_response(
      request_ch, response_ch, registry, std::move(req));

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

    dripcore::awaitable<std::string> execute(
        dripcore::task&,
        const std::string &,
        mistral::agent::agent_system &) override
    {
      co_return "";
    }
  };

  auto registry = std::make_shared<mistral::console::slash_command_registry>();
  registry->register_command(std::make_unique<empty_command>());

  SlashCommandExecutorTestFixture fixture;
  mistral::console::slash_command_request req;
  req.command_name = "empty_cmd";
  req.args = "";

  std::string response = fixture.execute_and_collect_response(
      request_ch, response_ch, registry, std::move(req));

  CHECK(response.empty());
}