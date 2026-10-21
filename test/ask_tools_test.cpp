#include "doctest/doctest.h"
#include "tools/ask_tools.h"

#include <dripcore/scheduler.h>
#include <dripcore/task.h>
#include <dripcore/awaitable.h>
#include <dripcore/channel.h>

#include <memory>
#include <string>
#include <vector>

namespace
{
  // Helper to extract the value type from an awaitable
  template <typename T>
  struct awaitable_value {
    using type = T;
  };

  template <typename T>
  struct awaitable_value<dripcore::awaitable<T>> {
    using type = T;
  };

  template <typename T>
  using awaitable_value_t = typename awaitable_value<T>::type;

  template <typename T>
  class async_result {
  public:
    async_result() : completed_(false) {}
    void set(T value) { value_ = std::move(value); completed_ = true; }
    T get() const { return std::move(value_); }
    bool completed() const { return completed_; }
  private:
    T value_;
    bool completed_;
  };

  template <typename Func>
  auto run_async_task_op(Func&& func) {
    using func_result = decltype(func());
    using result_type = awaitable_value_t<func_result>;

    async_result<result_type> result_holder;

    class async_task_op : public dripcore::task
    {
    public:
      async_task_op(Func&& f, async_result<result_type>& holder)
        : func_(std::forward<Func>(f)), holder_(holder) {}

      dripcore::awaitable<> body() override {
        if constexpr (std::is_same_v<func_result, dripcore::awaitable<result_type>>) {
          holder_.set(co_await func_());
        } else {
          holder_.set(func_());
        }
        co_return;
      }

    private:
      Func func_;
      async_result<result_type>& holder_;
    };

    dripcore::scheduler scheduler;
    scheduler.spawn<async_task_op>(std::forward<Func>(func), result_holder);
    scheduler.run();

    if (result_holder.completed()) {
      return result_holder.get();
    }
    return result_type{};
  }
}

TEST_CASE("ask_tools: no handler returns unavailable")
{
  mistral::tools::ask_tools tools;

  auto r = run_async_task_op([&]() {
    return tools.ask_user("Which option?", "", {}, "");
  });

  CHECK(r["status"] == "unavailable");
  CHECK(r.contains("message"));
}

TEST_CASE("ask_tools: answered returns the human's answer")
{
  mistral::tools::ask_tools tools;
  std::string captured_prompt;

  tools.set_prompt_handler(
    [&](const std::string& prompt) -> dripcore::awaitable<std::string> {
      captured_prompt = prompt;
      co_return std::string("cursor pagination");
    });

  auto r = run_async_task_op([&]() {
    return tools.ask_user(
      "Which pagination?", "The API offers both", {"cursor", "offset"}, "cursor");
  });

  CHECK(r["status"] == "answered");
  CHECK(r["answer"] == "cursor pagination");
  // Free-text answer: no option resolution.
  CHECK(r["matched_option"].is_null());

  // The prompt carries the transport prefix and the formatted question.
  CHECK(captured_prompt.find(mistral::tools::ask_prompt_prefix) == 0);
  CHECK(captured_prompt.find("Which pagination?") != std::string::npos);
  CHECK(captured_prompt.find("The API offers both") != std::string::npos);
  CHECK(captured_prompt.find("1. cursor") != std::string::npos);
  CHECK(captured_prompt.find("2. offset") != std::string::npos);
  CHECK(captured_prompt.find("Recommendation: cursor") != std::string::npos);
  CHECK(captured_prompt.find("Reply with the option number") != std::string::npos);
}

TEST_CASE("ask_tools: bare number answer resolves to matched_option")
{
  mistral::tools::ask_tools tools;

  tools.set_prompt_handler(
    [](const std::string&) -> dripcore::awaitable<std::string> {
      co_return std::string("2");
    });

  auto r = run_async_task_op([&]() {
    return tools.ask_user("Which pagination?", "", {"cursor", "offset"}, "");
  });

  CHECK(r["status"] == "answered");
  CHECK(r["answer"] == "2");
  CHECK(r["matched_option"] == "offset");

  // Surrounding whitespace is tolerated.
  tools.set_prompt_handler(
    [](const std::string&) -> dripcore::awaitable<std::string> {
      co_return std::string(" 1 ");
    });

  auto r2 = run_async_task_op([&]() {
    return tools.ask_user("Which pagination?", "", {"cursor", "offset"}, "");
  });

  CHECK(r2["matched_option"] == "cursor");
}

TEST_CASE("ask_tools: zero and out-of-range numbers do not resolve")
{
  mistral::tools::ask_tools tools;

  tools.set_prompt_handler(
    [](const std::string&) -> dripcore::awaitable<std::string> {
      co_return std::string("0");
    });

  auto r = run_async_task_op([&]() {
    return tools.ask_user("Pick one", "", {"a", "b"}, "");
  });

  CHECK(r["status"] == "answered");
  CHECK(r["answer"] == "0");
  CHECK(r["matched_option"].is_null());

  tools.set_prompt_handler(
    [](const std::string&) -> dripcore::awaitable<std::string> {
      co_return std::string("9");
    });

  auto r2 = run_async_task_op([&]() {
    return tools.ask_user("Pick one", "", {"a", "b"}, "");
  });

  CHECK(r2["status"] == "answered");
  CHECK(r2["matched_option"].is_null());
}

TEST_CASE("ask_tools: empty answer returns skipped")
{
  mistral::tools::ask_tools tools;

  tools.set_prompt_handler(
    [](const std::string&) -> dripcore::awaitable<std::string> {
      co_return std::string("");
    });

  auto r = run_async_task_op([&]() {
    return tools.ask_user("Proceed?", "", {}, "");
  });

  CHECK(r["status"] == "skipped");
  CHECK(r.contains("message"));
}

TEST_CASE("ask_tools: handler round-trip over real channels")
{
  // Exercises the same contract the interactive agent task registers:
  // the question goes out on the prompt channel, the human's answer comes
  // back on the input channel.
  auto prompt_ch = std::make_shared<dripcore::channel<std::string>>(1);
  auto input_ch = std::make_shared<dripcore::channel<std::string>>(1);

  mistral::tools::ask_tools tools;
  std::string received_prompt;
  mistral::tools::json result;

  // Plays the human: waits for the question on the prompt channel, then
  // answers on the input channel.
  class human_task : public dripcore::task
  {
  public:
    human_task(
      std::shared_ptr<dripcore::channel<std::string>> prompt_ch,
      std::shared_ptr<dripcore::channel<std::string>> input_ch,
      std::string& received_prompt,
      std::string answer)
      : prompt_ch_(std::move(prompt_ch))
      , input_ch_(std::move(input_ch))
      , received_prompt_(received_prompt)
      , answer_(std::move(answer))
    {}

    dripcore::awaitable<> body() override
    {
      auto prompt = co_await prompt_ch_->recv(this);
      if (prompt) {
        received_prompt_ = *prompt;
        input_ch_->try_send(answer_);
      }
      co_return;
    }

  private:
    std::shared_ptr<dripcore::channel<std::string>> prompt_ch_;
    std::shared_ptr<dripcore::channel<std::string>> input_ch_;
    std::string& received_prompt_;
    std::string answer_;
  };

  // Plays the interactive agent task: registers the prompt handler that
  // forwards the question to the prompt channel and suspends until the
  // answer arrives on the input channel.
  class agent_task : public dripcore::task
  {
  public:
    agent_task(
      std::shared_ptr<dripcore::channel<std::string>> prompt_ch,
      std::shared_ptr<dripcore::channel<std::string>> input_ch,
      mistral::tools::ask_tools& tools,
      mistral::tools::json& result)
      : prompt_ch_(std::move(prompt_ch))
      , input_ch_(std::move(input_ch))
      , tools_(tools)
      , result_(result)
    {
      tools_.set_prompt_handler(
        [this](const std::string& prompt) -> dripcore::awaitable<std::string> {
          prompt_ch_->try_send(prompt);
          auto response = co_await input_ch_->recv(this);
          co_return response.value_or("");
        });
    }

    dripcore::awaitable<> body() override
    {
      result_ = co_await tools_.ask_user(
        "Which option?", "Some context", {"a", "b"}, "a");
      co_return;
    }

  private:
    std::shared_ptr<dripcore::channel<std::string>> prompt_ch_;
    std::shared_ptr<dripcore::channel<std::string>> input_ch_;
    mistral::tools::ask_tools& tools_;
    mistral::tools::json& result_;
  };

  dripcore::scheduler scheduler;
  scheduler.spawn<human_task>(prompt_ch, input_ch, received_prompt, "option b");
  scheduler.spawn<agent_task>(prompt_ch, input_ch, tools, result);
  scheduler.run();

  CHECK(result["status"] == "answered");
  CHECK(result["answer"] == "option b");
  CHECK(result["matched_option"].is_null());
  CHECK(received_prompt.find(mistral::tools::ask_prompt_prefix) == 0);
  CHECK(received_prompt.find("Which option?") != std::string::npos);
}

TEST_CASE("ask_tools: prompt omits empty context, options and recommendation")
{
  mistral::tools::ask_tools tools;
  std::string captured_prompt;

  tools.set_prompt_handler(
    [&](const std::string& prompt) -> dripcore::awaitable<std::string> {
      captured_prompt = prompt;
      co_return std::string("ok");
    });

  auto r = run_async_task_op([&]() {
    return tools.ask_user("Question only?", "", {}, "");
  });

  CHECK(r["status"] == "answered");
  CHECK(captured_prompt.find("Question only?") != std::string::npos);
  CHECK(captured_prompt.find("Context:") == std::string::npos);
  CHECK(captured_prompt.find("Options:") == std::string::npos);
  CHECK(captured_prompt.find("Recommendation:") == std::string::npos);
  CHECK(captured_prompt.find("Reply with the option number") == std::string::npos);
}
