// DOCTEST unit tests for slash_command_executor_task
#include <doctest/doctest.h>

#include "console/slash_command_executor_task.h"
#include "console/slash_command_registry.h"
#include "agent/agent_system.h"

#include <dripcore/channel.h>
#include <dripcore/scheduler.h>

#include <string>
#include <thread>
#include <unistd.h>
#include <arpa/inet.h>

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

    std::string execute_agent(const std::string& args, mistral::agent::agent_system&) override
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

    std::string execute_immediate(const std::string& args) override
    {
        return "Immediate: " + args;
    }
};

} // anonymous namespace

namespace
{

std::string read_length_prefixed(int fd)
{
    uint32_t len;
    ssize_t n = ::read(fd, &len, sizeof(len));
    if (n != static_cast<ssize_t>(sizeof(len)))
    {
        return "";
    }
    len = ntohl(len);
    if (len == 0) return "";

    std::string response;
    response.resize(len);
    ssize_t bytes_read = 0;
    while (bytes_read < static_cast<ssize_t>(len))
    {
        ssize_t chunk = ::read(fd, response.data() + bytes_read, len - bytes_read);
        if (chunk <= 0) break;
        bytes_read += chunk;
    }
    return (bytes_read == static_cast<ssize_t>(len)) ? response : "";
}

} // anonymous namespace

TEST_CASE("slash_command_executor_task: execute AGENT command")
{
    int fds[2];
    REQUIRE(::pipe(fds) == 0);

    auto request_ch = std::make_shared<dripcore::channel<mistral::console::slash_command_request>>(1);

    mistral::console::slash_command_executor_task executor(request_ch, fds[1]);

    auto& registry = mistral::console::slash_command_registry::instance();
    registry.register_command(std::make_unique<executor_test_agent_command>());

    dripcore::scheduler scheduler;
    scheduler.spawn<mistral::console::slash_command_executor_task>(executor);

    mistral::console::slash_command_request req;
    req.command_name = "test_cmd";
    req.args = "hello";
    request_ch->try_send(std::move(req));
    request_ch->close();

    scheduler.run();

    std::string response = read_length_prefixed(fds[0]);
    CHECK(response == "Response: hello");

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST_CASE("slash_command_executor_task: unknown command")
{
    int fds[2];
    REQUIRE(::pipe(fds) == 0);

    auto request_ch = std::make_shared<dripcore::channel<mistral::console::slash_command_request>>(1);
    mistral::console::slash_command_executor_task executor(request_ch, fds[1]);

    dripcore::scheduler scheduler;
    scheduler.spawn<mistral::console::slash_command_executor_task>(executor);

    mistral::console::slash_command_request req;
    req.command_name = "unknown_cmd";
    req.args = "";
    request_ch->try_send(std::move(req));
    request_ch->close();

    scheduler.run();

    std::string response = read_length_prefixed(fds[0]);
    CHECK(response.find("Unknown command: /unknown_cmd") != std::string::npos);

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST_CASE("slash_command_executor_task: IMMEDIATE command in AGENT executor")
{
    int fds[2];
    REQUIRE(::pipe(fds) == 0);

    auto request_ch = std::make_shared<dripcore::channel<mistral::console::slash_command_request>>(1);
    mistral::console::slash_command_executor_task executor(request_ch, fds[1]);

    auto& registry = mistral::console::slash_command_registry::instance();
    registry.register_command(std::make_unique<executor_test_immediate_command>());

    dripcore::scheduler scheduler;
    scheduler.spawn<mistral::console::slash_command_executor_task>(executor);

    mistral::console::slash_command_request req;
    req.command_name = "immediate_cmd";
    req.args = "";
    request_ch->try_send(std::move(req));
    request_ch->close();

    scheduler.run();

    std::string response = read_length_prefixed(fds[0]);
    CHECK(response.find("must be executed in IMMEDIATE context") != std::string::npos);

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST_CASE("slash_command_executor_task: exception handling")
{
    int fds[2];
    REQUIRE(::pipe(fds) == 0);

    auto request_ch = std::make_shared<dripcore::channel<mistral::console::slash_command_request>>(1);
    mistral::console::slash_command_executor_task executor(request_ch, fds[1]);

    class throwing_command : public mistral::console::slash_command
    {
    public:
        std::string name() const override { return "throw_cmd"; }
        std::string description() const override { return "Throws"; }
        execution_context context() const override { return execution_context::AGENT; }
        std::string execute_agent(const std::string&, mistral::agent::agent_system&) override
        {
            throw std::runtime_error("Test error");
        }
    };

    auto& registry = mistral::console::slash_command_registry::instance();
    registry.register_command(std::make_unique<throwing_command>());

    dripcore::scheduler scheduler;
    scheduler.spawn<mistral::console::slash_command_executor_task>(executor);

    mistral::console::slash_command_request req;
    req.command_name = "throw_cmd";
    req.args = "";
    request_ch->try_send(std::move(req));
    request_ch->close();

    scheduler.run();

    std::string response = read_length_prefixed(fds[0]);
    CHECK(response.find("Error executing /throw_cmd: Test error") != std::string::npos);

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST_CASE("slash_command_executor_task: empty response")
{
    int fds[2];
    REQUIRE(::pipe(fds) == 0);

    auto request_ch = std::make_shared<dripcore::channel<mistral::console::slash_command_request>>(1);
    mistral::console::slash_command_executor_task executor(request_ch, fds[1]);

    class empty_command : public mistral::console::slash_command
    {
    public:
        std::string name() const override { return "empty_cmd"; }
        std::string description() const override { return "Empty"; }
        execution_context context() const override { return execution_context::AGENT; }
        std::string execute_agent(const std::string&, mistral::agent::agent_system&) override
        {
            return "";
        }
    };

    auto& registry = mistral::console::slash_command_registry::instance();
    registry.register_command(std::make_unique<empty_command>());

    dripcore::scheduler scheduler;
    scheduler.spawn<mistral::console::slash_command_executor_task>(executor);

    mistral::console::slash_command_request req;
    req.command_name = "empty_cmd";
    req.args = "";
    request_ch->try_send(std::move(req));
    request_ch->close();

    scheduler.run();

    std::string response = read_length_prefixed(fds[0]);
    CHECK(response.empty());

    ::close(fds[0]);
    ::close(fds[1]);
}