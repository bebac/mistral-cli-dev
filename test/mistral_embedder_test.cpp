#include <doctest/doctest.h>
#ifdef MISTRAL_ENABLE_KNOWLEDGE

#include "api/types.h"
#include "knowledge/hashing_embedder.h"
#include "knowledge/mistral_embedder.h"

#include <dripcore/channel.h>
#include <dripcore/scheduler.h>
#include <dripcore/task.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace api = mistral::api;

namespace
{
  // ---- Async test helpers ----

  template <typename T> struct awaitable_value
  {
    using type = T;
  };

  template <typename T> struct awaitable_value<dripcore::awaitable<T>>
  {
    using type = T;
  };

  template <typename T> using awaitable_value_t = typename awaitable_value<T>::type;

  template <typename T> class async_result
  {
  public:
    void set(T value)
    {
      value_ = std::move(value);
      completed_ = true;
    }
    T get() const
    {
      return value_;
    }
    bool completed() const
    {
      return completed_;
    }

  private:
    T value_{};
    bool completed_ = false;
  };

  // Run `func` on a fresh scheduler, after letting `setup` spawn any
  // extra tasks (e.g. a fake coordinator) on the same scheduler.
  template <typename Func, typename Setup> auto run_with(Func&& func, Setup&& setup)
  {
    using func_result = decltype(func());
    using result_type = awaitable_value_t<func_result>;

    async_result<result_type> result_holder;

    class async_task : public dripcore::task
    {
    public:
      async_task(Func&& f, async_result<result_type>& holder) : func_(std::move(f)), holder_(holder)
      {
      }

      dripcore::awaitable<> body() override
      {
        holder_.set(co_await func_());
        co_return;
      }

    private:
      Func func_;
      async_result<result_type>& holder_;
    };

    dripcore::scheduler scheduler;
    setup(scheduler);
    scheduler.spawn<async_task>(std::forward<Func>(func), result_holder);
    scheduler.run();

    REQUIRE(result_holder.completed());
    return result_holder.get();
  }

  template <typename Func> auto run_async(Func&& func)
  {
    return run_with(std::forward<Func>(func), [](dripcore::scheduler&) {});
  }

  // ---- Fake transport ----

  struct request_capture
  {
    std::string path;
    std::string body;
    bool stream = true;
    bool answered = false;
  };

  // Stands in for http_coordinator_task: answers one api_request with a
  // canned non-streaming response, the way stream_response_state does
  // (one data event carrying the whole body, then done).
  class fake_coordinator : public dripcore::task
  {
  public:
    fake_coordinator(
      std::shared_ptr<dripcore::channel<api::api_request>> request_ch,
      std::string response_body,
      std::string error_payload,
      request_capture* capture
    )
      : request_ch_(std::move(request_ch)), response_body_(std::move(response_body)),
        error_payload_(std::move(error_payload)), capture_(capture)
    {
    }

  protected:
    dripcore::awaitable<> body() override
    {
      auto req = co_await request_ch_->recv(this);
      if (!req)
      {
        co_return;
      }
      if (capture_)
      {
        capture_->path = req->path;
        capture_->body = req->body;
        capture_->stream = req->stream;
      }
      if (!error_payload_.empty())
      {
        req->response_ch->try_send({api::event_type::error, error_payload_});
      }
      else
      {
        req->response_ch->try_send({api::event_type::data, response_body_});
      }
      req->response_ch->try_send({api::event_type::done, {}});
      if (capture_)
      {
        capture_->answered = true;
      }
      co_return;
    }

  private:
    std::shared_ptr<dripcore::channel<api::api_request>> request_ch_;
    std::string response_body_;
    std::string error_payload_;
    request_capture* capture_;
  };

  // Accepts the request and never answers, standing in for a stalled
  // HTTP/2 stream that never reaches stream_closed.
  class silent_coordinator : public dripcore::task
  {
  public:
    explicit silent_coordinator(std::shared_ptr<dripcore::channel<api::api_request>> request_ch)
      : request_ch_(std::move(request_ch))
    {
    }

  protected:
    dripcore::awaitable<> body() override
    {
      auto req = co_await request_ch_->recv(this);
      (void)req; // deliberately unanswered
      co_return;
    }

  private:
    std::shared_ptr<dripcore::channel<api::api_request>> request_ch_;
  };

  // Accepts the request and closes the response channel without sending
  // any event, standing in for a coordinator that gives up.
  class closing_coordinator : public dripcore::task
  {
  public:
    explicit closing_coordinator(std::shared_ptr<dripcore::channel<api::api_request>> request_ch)
      : request_ch_(std::move(request_ch))
    {
    }

  protected:
    dripcore::awaitable<> body() override
    {
      auto req = co_await request_ch_->recv(this);
      if (req && req->response_ch)
      {
        req->response_ch->close();
      }
      co_return;
    }

  private:
    std::shared_ptr<dripcore::channel<api::api_request>> request_ch_;
  };

  // Answers a fixed number of requests, recording how many inputs each
  // carried. Exits after the expected count so the scheduler can finish.
  class multi_coordinator : public dripcore::task
  {
  public:
    multi_coordinator(
      std::shared_ptr<dripcore::channel<api::api_request>> request_ch,
      int expected_requests,
      std::vector<std::size_t>* batch_sizes
    )
      : request_ch_(std::move(request_ch)), expected_requests_(expected_requests), batch_sizes_(batch_sizes)
    {
    }

  protected:
    dripcore::awaitable<> body() override
    {
      for (int i = 0; i < expected_requests_; ++i)
      {
        auto req = co_await request_ch_->recv(this);
        if (!req)
        {
          co_return;
        }
        const auto parsed = api::json::parse(req->body);
        const std::size_t count = parsed["input"].size();
        batch_sizes_->push_back(count);

        api::json data = api::json::array();
        for (std::size_t j = 0; j < count; ++j)
        {
          data.push_back({
            {"object", "embedding"},
            {"index", static_cast<int>(j)},
            {"embedding", std::vector<float>{0.5f, 0.5f}},
          });
        }
        api::json body = {
          {"id", "test"},
          {"object", "list"},
          {"model", "codestral-embed-2505"},
          {"data", std::move(data)},
          {"usage", {{"prompt_tokens", 1}}},
        };
        req->response_ch->try_send({api::event_type::data, body.dump()});
        req->response_ch->try_send({api::event_type::done, {}});
      }
      co_return;
    }

  private:
    std::shared_ptr<dripcore::channel<api::api_request>> request_ch_;
    int expected_requests_;
    std::vector<std::size_t>* batch_sizes_;
  };

  // Response body for the given vectors, emitted in reverse order so the
  // tests prove that data[].index is what orders the result.
  std::string make_response(const std::vector<std::vector<float>>& vectors)
  {
    api::json data = api::json::array();
    for (std::size_t i = vectors.size(); i-- > 0;)
    {
      data.push_back({
        {"object", "embedding"},
        {"index", static_cast<int>(i)},
        {"embedding", vectors[i]},
      });
    }
    api::json body = {
      {"id", "test"},
      {"object", "list"},
      {"model", "codestral-embed-2505"},
      {"data", std::move(data)},
      {"usage", {{"prompt_tokens", 4}}},
    };
    return body.dump();
  }
} // namespace

// ---- Wire format ----

TEST_CASE("build_embeddings_request: shape and optional fields")
{
  using mistral::knowledge::build_embeddings_request;

  const auto body =
    api::json::parse(build_embeddings_request("codestral-embed-2505", {"alpha", "beta"}, 1536, "float"));
  CHECK(body["model"].get<std::string>() == "codestral-embed-2505");
  REQUIRE(body["input"].is_array());
  REQUIRE(body["input"].size() == 2);
  CHECK(body["input"][0].get<std::string>() == "alpha");
  CHECK(body["input"][1].get<std::string>() == "beta");
  CHECK(body["output_dimension"].get<int>() == 1536);

  // EmbeddingRequest is additionalProperties: false, and "float" is the
  // server default, so neither extra fields nor output_dtype are sent.
  CHECK(body.size() == 3);
  CHECK_FALSE(body.contains("output_dtype"));

  const auto no_dim = api::json::parse(build_embeddings_request("m", {"x"}, 0, ""));
  CHECK_FALSE(no_dim.contains("output_dimension"));
  CHECK_FALSE(no_dim.contains("output_dtype"));
  CHECK(no_dim.size() == 2);

  const auto int8 = api::json::parse(build_embeddings_request("m", {"x"}, 512, "int8"));
  CHECK(int8["output_dimension"].get<int>() == 512);
  CHECK(int8["output_dtype"].get<std::string>() == "int8");
}

TEST_CASE("parse_embeddings_response: orders by index")
{
  using mistral::knowledge::parse_embeddings_response;

  std::vector<std::vector<float>> out;
  std::string error;

  const std::string body = R"({"data":[
    {"index":1,"embedding":[0.5,0.25]},
    {"index":0,"embedding":[1.0,0.0]}
  ]})";
  REQUIRE(parse_embeddings_response(body, 2, out, error));
  CHECK(error.empty());
  REQUIRE(out.size() == 2);
  CHECK(out[0][0] == doctest::Approx(1.0f));
  CHECK(out[0][1] == doctest::Approx(0.0f));
  CHECK(out[1][0] == doctest::Approx(0.5f));
  CHECK(out[1][1] == doctest::Approx(0.25f));
}

TEST_CASE("parse_embeddings_response: rejects malformed responses")
{
  using mistral::knowledge::parse_embeddings_response;

  std::vector<std::vector<float>> out;
  std::string error;

  // Count mismatch.
  CHECK_FALSE(parse_embeddings_response(R"({"data":[{"index":0,"embedding":[1]}]})", 2, out, error));
  CHECK(error.find("expected 2") != std::string::npos);

  // An error payload is surfaced verbatim: the endpoint's messages are
  // already actionable ("output_dimension must be less than or equal to
  // 3072.").
  error.clear();
  CHECK_FALSE(parse_embeddings_response(
    R"({"object":"error","message":"output_dimension must be less than or equal to 3072."})", 1, out, error
  ));
  CHECK(error == "output_dimension must be less than or equal to 3072.");

  // Repeated index.
  error.clear();
  CHECK_FALSE(parse_embeddings_response(
    R"({"data":[{"index":0,"embedding":[1]},{"index":0,"embedding":[2]}]})", 2, out, error
  ));
  CHECK(error.find("repeats index") != std::string::npos);

  // Out-of-range index.
  error.clear();
  CHECK_FALSE(parse_embeddings_response(R"({"data":[{"index":5,"embedding":[1]}]})", 1, out, error));
  CHECK(error.find("out of range") != std::string::npos);

  // Mixed dimensions: the vec0 column is fixed.
  error.clear();
  CHECK_FALSE(parse_embeddings_response(
    R"({"data":[{"index":0,"embedding":[1,2]},{"index":1,"embedding":[1]}]})", 2, out, error
  ));
  CHECK(error.find("mixes dimensions") != std::string::npos);

  // Not JSON at all.
  error.clear();
  CHECK_FALSE(parse_embeddings_response("<html>", 1, out, error));
  CHECK(error.find("not valid JSON") != std::string::npos);

  // int8 responses arrive as JSON integers and widen to float.
  error.clear();
  REQUIRE(parse_embeddings_response(R"({"data":[{"index":0,"embedding":[-14,6,3]}]})", 1, out, error));
  REQUIRE(out.size() == 1);
  CHECK(out[0][0] == doctest::Approx(-14.0f));
  CHECK(out[0][2] == doctest::Approx(3.0f));
}

// ---- Local embedder ----

TEST_CASE("hashing_embedder: implements the embedder interface")
{
  mistral::knowledge::hashing_embedder embedder(64);
  CHECK(embedder.dim() == 64);
  CHECK(embedder.identity() == "local-hash/64");

  std::string error;
  const auto vectors = run_async([&]() { return embedder.embed_batch({"alpha", "beta"}, error); });
  CHECK(error.empty());
  REQUIRE(vectors.size() == 2);
  CHECK(vectors[0].size() == 64);
  CHECK(vectors[1].size() == 64);

  // An empty batch is rejected rather than silently accepted.
  error.clear();
  const auto empty = run_async([&]() { return embedder.embed_batch(std::vector<std::string>{}, error); });
  CHECK(empty.empty());
  CHECK_FALSE(error.empty());
}

// ---- Remote embedder ----

TEST_CASE("mistral_embedder: sends a batched request and parses the response")
{
  auto request_ch = std::make_shared<dripcore::channel<api::api_request>>(4);
  const std::vector<std::vector<float>> expected = {{1.0f, 0.0f}, {0.5f, 0.25f}};
  request_capture capture;

  mistral::knowledge::mistral_embedder embedder(request_ch, "codestral-embed-2505", "v1", 2, "float");
  CHECK(embedder.dim() == 2);
  CHECK(embedder.identity() == "mistral/codestral-embed-2505/2/float");

  // Default configured batch size, clamped to the endpoint's hard limit.
  CHECK(embedder.batch_size() == 32);
  mistral::knowledge::mistral_embedder oversized(request_ch, "m", "v1", 2, "float", 999);
  CHECK(oversized.batch_size() == 256);

  std::string error;
  const auto vectors = run_with(
    [&]() { return embedder.embed_batch({"alpha", "beta"}, error); },
    [&](dripcore::scheduler& scheduler) {
      scheduler.spawn<fake_coordinator>(request_ch, make_response(expected), std::string{}, &capture);
    }
  );

  CHECK(error.empty());
  REQUIRE(vectors.size() == 2);
  CHECK(vectors[0][0] == doctest::Approx(1.0f));
  CHECK(vectors[1][1] == doctest::Approx(0.25f));

  CHECK(capture.answered);
  CHECK(capture.path == "/v1/embeddings");
  CHECK_FALSE(capture.stream);

  const auto body = api::json::parse(capture.body);
  CHECK(body["model"].get<std::string>() == "codestral-embed-2505");
  REQUIRE(body["input"].size() == 2);
  CHECK(body["output_dimension"].get<int>() == 2);
}

TEST_CASE("mistral_embedder: surfaces transport errors")
{
  auto request_ch = std::make_shared<dripcore::channel<api::api_request>>(4);
  request_capture capture;

  mistral::knowledge::mistral_embedder embedder(request_ch, "m", "v1", 2, "float");

  std::string error;
  const auto vectors = run_with(
    [&]() { return embedder.embed_batch({"alpha"}, error); },
    [&](dripcore::scheduler& scheduler) {
      scheduler.spawn<fake_coordinator>(request_ch, std::string{}, std::string("HTTP 400: bad"), &capture);
    }
  );

  CHECK(vectors.empty());
  CHECK(error == "HTTP 400: bad");
}

TEST_CASE("mistral_embedder: reports a missing transport")
{
  mistral::knowledge::mistral_embedder embedder(nullptr, "m", "v1", 2, "float");

  std::string error;
  const auto vectors = run_async([&]() { return embedder.embed_batch({"alpha"}, error); });
  CHECK(vectors.empty());
  CHECK(error.find("no transport channel") != std::string::npos);
}

TEST_CASE("mistral_embedder: rejects oversized batches")
{
  auto request_ch = std::make_shared<dripcore::channel<api::api_request>>(4);
  mistral::knowledge::mistral_embedder embedder(request_ch, "m", "v1", 2, "float");

  // 256 inputs succeed against the live endpoint; 512 are rejected.
  const std::vector<std::string> inputs(257, "x");

  std::string error;
  const auto vectors = run_async([&]() { return embedder.embed_batch(inputs, error); });
  CHECK(vectors.empty());
  CHECK(error.find("at most 256") != std::string::npos);
}

TEST_CASE("mistral_embedder: a stalled response times out instead of hanging")
{
  auto request_ch = std::make_shared<dripcore::channel<api::api_request>>(4);

  // 50 ms deadline so the test is fast; the default is 60 s.
  mistral::knowledge::mistral_embedder embedder(request_ch, "m", "v1", 2, "float", 32, 50);

  std::string error;
  const auto vectors = run_with(
    [&]() { return embedder.embed_batch({"alpha"}, error); },
    [&](dripcore::scheduler& scheduler) { scheduler.spawn<silent_coordinator>(request_ch); }
  );

  CHECK(vectors.empty());
  CHECK(error.find("timed out") != std::string::npos);
}

TEST_CASE("mistral_embedder: a closed response channel fails fast")
{
  auto request_ch = std::make_shared<dripcore::channel<api::api_request>>(4);

  // A long deadline: the closed-channel check must fire well before it.
  mistral::knowledge::mistral_embedder embedder(request_ch, "m", "v1", 2, "float", 32, 60000);

  std::string error;
  const auto vectors = run_with(
    [&]() { return embedder.embed_batch({"alpha"}, error); },
    [&](dripcore::scheduler& scheduler) { scheduler.spawn<closing_coordinator>(request_ch); }
  );

  CHECK(vectors.empty());
  CHECK(error.find("closed without a response") != std::string::npos);
}

TEST_CASE("mistral_embedder: splits a batch that would exceed the request token budget")
{
  auto request_ch = std::make_shared<dripcore::channel<api::api_request>>(4);
  std::vector<std::size_t> batch_sizes;

  // Three 20 KB inputs against a 40 KB per-request budget: 2 then 1.
  // batch_size (32) alone would have sent all three in one request,
  // which the endpoint rejects with "Too many tokens overall".
  mistral::knowledge::mistral_embedder embedder(request_ch, "m", "v1", 2, "float", 32, 60000, 40000);

  std::vector<std::string> inputs(3, std::string(20000, 'x'));

  std::string error;
  const auto vectors = run_with(
    [&]() { return embedder.embed_batch(inputs, error); },
    [&](dripcore::scheduler& scheduler) { scheduler.spawn<multi_coordinator>(request_ch, 2, &batch_sizes); }
  );

  CHECK(error.empty());
  CHECK(vectors.size() == 3);
  REQUIRE(batch_sizes.size() == 2);
  CHECK(batch_sizes[0] == 2);
  CHECK(batch_sizes[1] == 1);
}

#endif // MISTRAL_ENABLE_KNOWLEDGE
