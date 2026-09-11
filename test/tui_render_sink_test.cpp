#include "tui/tui_thread_safe_queue.h"
#include "tui/tui_render_sink.h"
#include "doctest/doctest.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Test harness for tui_render_sink: captures posted callbacks, appended
// fragments, and flush calls. Allows the test to control when drain
// callbacks execute (simulating the UI event loop).
// ---------------------------------------------------------------------------
namespace {
  using content_type = mistral::tui::content_type;

  struct sink_test_harness {
    std::mutex mutex;
    std::vector<std::pair<std::string, content_type>> appended;
    int flush_count = 0;
    int completed_count = 0;
    std::vector<std::function<void()>> posted;
    int post_count = 0;

    mistral::tui::tui_render_sink::post_fn make_post() {
      return [this](std::function<void()> cb) {
        std::lock_guard<std::mutex> lock(mutex);
        posted.push_back(std::move(cb));
        ++post_count;
      };
    }

    mistral::tui::tui_render_sink::append_fn make_append() {
      return [this](const std::string& text, content_type type) {
        std::lock_guard<std::mutex> lock(mutex);
        appended.emplace_back(text, type);
      };
    }

    mistral::tui::tui_render_sink::flush_fn make_flush() {
      return [this]() {
        std::lock_guard<std::mutex> lock(mutex);
        ++flush_count;
      };
    }

    mistral::tui::tui_render_sink::completed_fn make_completed() {
      return [this]() {
        std::lock_guard<std::mutex> lock(mutex);
        ++completed_count;
      };
    }

    bool run_one_posted() {
      std::function<void()> cb;
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (posted.empty()) return false;
        cb = std::move(posted.front());
        posted.erase(posted.begin());
      }
      cb();
      return true;
    }

    void run_all_posted() {
      while (run_one_posted()) {}
    }
  };
} // namespace

// ===========================================================================
// thread_safe_queue tests
// ===========================================================================

TEST_SUITE_BEGIN("thread_safe_queue");

TEST_CASE("push and drain returns items in FIFO order") {
  mistral::tui::thread_safe_queue<int> q;
  q.push(1);
  q.push(2);
  q.push(3);
  auto items = q.drain();
  CHECK(items.size() == 3);
  CHECK(items[0] == 1);
  CHECK(items[1] == 2);
  CHECK(items[2] == 3);
}

TEST_CASE("drain on empty queue returns empty deque") {
  mistral::tui::thread_safe_queue<int> q;
  auto items = q.drain();
  CHECK(items.empty());
}

TEST_CASE("drain clears the queue") {
  mistral::tui::thread_safe_queue<int> q;
  q.push(1);
  q.push(2);
  auto first = q.drain();
  CHECK(first.size() == 2);
  auto second = q.drain();
  CHECK(second.empty());
}

TEST_CASE("push after drain works") {
  mistral::tui::thread_safe_queue<int> q;
  q.push(1);
  q.drain();
  q.push(2);
  auto items = q.drain();
  CHECK(items.size() == 1);
  CHECK(items[0] == 2);
}

TEST_CASE("concurrent push from multiple threads") {
  mistral::tui::thread_safe_queue<int> q;
  constexpr int N = 1000;
  constexpr int T = 4;
  std::vector<std::thread> threads;
  for (int t = 0; t < T; ++t) {
    threads.emplace_back([&q, t]() {
      for (int i = 0; i < N; ++i) {
        q.push(t * N + i);
      }
    });
  }
  for (auto& th : threads) th.join();
  auto items = q.drain();
  CHECK(items.size() == static_cast<size_t>(N * T));
}

TEST_SUITE_END();

// ===========================================================================
// tui_render_sink tests
// ===========================================================================

TEST_SUITE_BEGIN("tui_render_sink");

TEST_CASE("single output call posts one drain callback") {
  sink_test_harness h;
  mistral::tui::tui_render_sink sink(
    h.make_post(), h.make_append(), h.make_flush());

  sink.output("hello");
  CHECK(h.post_count == 1);

  h.run_all_posted();

  CHECK(h.appended.size() == 1);
  CHECK(h.appended[0].first == "hello");
  CHECK(h.appended[0].second == content_type::normal);
  CHECK(h.flush_count == 1);
}

TEST_CASE("multiple outputs before drain batch into single post") {
  sink_test_harness h;
  mistral::tui::tui_render_sink sink(
    h.make_post(), h.make_append(), h.make_flush());

  sink.output("a");
  sink.output("b");
  sink.output("c");

  CHECK(h.post_count == 1);

  h.run_all_posted();

  CHECK(h.appended.size() == 3);
  CHECK(h.appended[0].first == "a");
  CHECK(h.appended[1].first == "b");
  CHECK(h.appended[2].first == "c");
  CHECK(h.flush_count == 1);
}

TEST_CASE("pending flag is cleared after drain completes") {
  sink_test_harness h;
  mistral::tui::tui_render_sink sink(
    h.make_post(), h.make_append(), h.make_flush());

  sink.output("a");
  CHECK(h.post_count == 1);
  h.run_all_posted();
  CHECK(h.appended.size() == 1);

  // After drain, pending should be false.
  // A new output should post a new callback.
  int post_count_before = h.post_count;
  sink.output("b");
  CHECK(h.post_count == post_count_before + 1);

  h.run_all_posted();
  CHECK(h.appended.size() == 2);
}

TEST_CASE("pending flag correctly managed across multiple drain cycles") {
  sink_test_harness h;
  mistral::tui::tui_render_sink sink(
    h.make_post(), h.make_append(), h.make_flush());

  // Cycle 1: single item
  sink.output("a");
  CHECK(h.post_count == 1);
  h.run_all_posted();
  CHECK(h.appended.size() == 1);

  // Cycle 2: two items, single post
  sink.output("b");
  sink.output("c");
  CHECK(h.post_count == 2);
  h.run_all_posted();
  CHECK(h.appended.size() == 3);

  // Cycle 3: single item again
  sink.output("d");
  CHECK(h.post_count == 3);
  h.run_all_posted();
  CHECK(h.appended.size() == 4);
}

TEST_CASE("re-post when items arrive during drain") {
  std::mutex mutex;
  std::vector<std::pair<std::string, content_type>> appended;
  int flush_count = 0;
  std::vector<std::function<void()>> posted;
  int post_count = 0;

  mistral::tui::tui_render_sink* sink_ptr = nullptr;

  auto post = [&](std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(mutex);
    posted.push_back(std::move(cb));
    ++post_count;
  };

  auto append = [&](const std::string& text, content_type type) {
    std::lock_guard<std::mutex> lock(mutex);
    appended.emplace_back(text, type);
    // When "a" is appended, enqueue "b" from within the drain
    // callback. This simulates new items arriving during drain
    // processing. Since pending is still true, output("b") will
    // not post a new callback. After drain finishes, it finds
    // "b" in the buffer and re-posts.
    if (text == "a" && sink_ptr) {
      sink_ptr->output("b");
    }
  };

  auto flush = [&]() {
    std::lock_guard<std::mutex> lock(mutex);
    ++flush_count;
  };

  mistral::tui::tui_render_sink sink(post, append, flush);
  sink_ptr = &sink;

  sink.output("a");
  CHECK(post_count == 1);

  // Run the first posted drain callback. During drain, "a" is
  // appended, which triggers output("b"). Since pending is still
  // true, output("b") doesn't post. After drain finishes, it
  // finds "b" in the buffer and re-posts.
  {
    std::function<void()> cb;
    {
      std::lock_guard<std::mutex> lock(mutex);
      REQUIRE(!posted.empty());
      cb = std::move(posted.front());
      posted.erase(posted.begin());
    }
    cb();
  }

  CHECK(post_count == 2);

  // Run the second drain callback for "b".
  {
    std::function<void()> cb;
    {
      std::lock_guard<std::mutex> lock(mutex);
      REQUIRE(!posted.empty());
      cb = std::move(posted.front());
      posted.erase(posted.begin());
    }
    cb();
  }

  CHECK(appended.size() == 2);
  CHECK(appended[0].first == "a");
  CHECK(appended[1].first == "b");
  CHECK(flush_count == 2);

  // No more pending callbacks.
  {
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(posted.empty());
  }
}

TEST_CASE("content type routing") {
  sink_test_harness h;
  mistral::tui::tui_render_sink sink(
    h.make_post(), h.make_append(), h.make_flush());

  sink.output("normal text");
  sink.output_thinking("thinking text");
  sink.output_line("line text");
  sink.output_tool_call("ls", "path=\".\"");
  sink.output_tool_result("result");
  sink.output_error("error text");
  sink.output_warning("warning text");
  sink.thinking_begin();
  sink.thinking_end();
  sink.completed();
  sink.cancelled();

  h.run_all_posted();

  REQUIRE(h.appended.size() == 11);

  CHECK(h.appended[0].first == "normal text");
  CHECK(h.appended[0].second == content_type::normal);

  CHECK(h.appended[1].first == "thinking text");
  CHECK(h.appended[1].second == content_type::thinking);

  CHECK(h.appended[2].first == "line text\n");
  CHECK(h.appended[2].second == content_type::tool_result);

  CHECK(h.appended[3].first == "\n[tool] ls(path=\".\")");
  CHECK(h.appended[3].second == content_type::tool_call);

  // " \xe2\x86\x92 " is " → " in UTF-8
  CHECK(h.appended[4].first == " \xe2\x86\x92 result\n");
  CHECK(h.appended[4].second == content_type::tool_result);

  CHECK(h.appended[5].first == "\n[error] error text\n");
  CHECK(h.appended[5].second == content_type::error);

  CHECK(h.appended[6].first == "\n[warning] warning text\n");
  CHECK(h.appended[6].second == content_type::warning);

  CHECK(h.appended[7].first == "\n");
  CHECK(h.appended[7].second == content_type::normal);

  CHECK(h.appended[8].first == "\n\n");
  CHECK(h.appended[8].second == content_type::normal);

  CHECK(h.appended[9].first == "\n");
  CHECK(h.appended[9].second == content_type::normal);

  CHECK(h.appended[10].first == "\n[cancelled]\n");
  CHECK(h.appended[10].second == content_type::warning);
}

TEST_CASE("output_tool_result truncates long results") {
  sink_test_harness h;
  mistral::tui::tui_render_sink sink(
    h.make_post(), h.make_append(), h.make_flush());

  std::string long_result(300, 'x');
  sink.output_tool_result(long_result);

  h.run_all_posted();

  REQUIRE(h.appended.size() == 1);
  // " \xe2\x86\x92 " (5 bytes) + 200 chars + "..." (3 bytes) + "\n" (1 byte) = 209
  CHECK(h.appended[0].first.size() == 209);
  CHECK(h.appended[0].first.substr(0, 5) == " \xe2\x86\x92 ");
  CHECK(h.appended[0].first.substr(5, 200) == std::string(200, 'x'));
  CHECK(h.appended[0].first.substr(205) == "...\n");
}

TEST_CASE("output_tool_result does not truncate short results") {
  sink_test_harness h;
  mistral::tui::tui_render_sink sink(
    h.make_post(), h.make_append(), h.make_flush());

  sink.output_tool_result("short");

  h.run_all_posted();

  REQUIRE(h.appended.size() == 1);
  CHECK(h.appended[0].first == " \xe2\x86\x92 short\n");
}

TEST_CASE("output_tool_result boundary at exactly 200 chars") {
  sink_test_harness h;
  mistral::tui::tui_render_sink sink(
    h.make_post(), h.make_append(), h.make_flush());

  std::string exact_200(200, 'x');
  sink.output_tool_result(exact_200);

  h.run_all_posted();

  REQUIRE(h.appended.size() == 1);
  // 200 is not > 200, so no truncation
  CHECK(h.appended[0].first == " \xe2\x86\x92 " + exact_200 + "\n");
}

TEST_CASE("output_tool_result truncates at 201 chars") {
  sink_test_harness h;
  mistral::tui::tui_render_sink sink(
    h.make_post(), h.make_append(), h.make_flush());

  std::string result_201(201, 'x');
  sink.output_tool_result(result_201);

  h.run_all_posted();

  REQUIRE(h.appended.size() == 1);
  std::string expected = " \xe2\x86\x92 " + std::string(200, 'x') + "...\n";
  CHECK(h.appended[0].first == expected);
}

TEST_CASE("concurrent enqueue from multiple threads") {
  sink_test_harness h;
  mistral::tui::tui_render_sink sink(
    h.make_post(), h.make_append(), h.make_flush());

  constexpr int N = 100;
  constexpr int T = 4;
  std::vector<std::thread> threads;

  for (int t = 0; t < T; ++t) {
    threads.emplace_back([&sink, t]() {
      for (int i = 0; i < N; ++i) {
        sink.output(std::to_string(t * N + i));
      }
    });
  }
  for (auto& th : threads) th.join();

  // Drain all posted callbacks. May need multiple rounds
  // because re-posting can happen.
  h.run_all_posted();

  CHECK(h.appended.size() == static_cast<size_t>(N * T));
  CHECK(h.flush_count >= 1);
}

TEST_CASE("completed_fn is called after drain when completed() is invoked") {
  sink_test_harness h;
  mistral::tui::tui_render_sink sink(
    h.make_post(), h.make_append(), h.make_flush(),
    h.make_completed());

  sink.output("hello");
  sink.completed();

  h.run_all_posted();

  CHECK(h.appended.size() >= 1);
  CHECK(h.completed_count == 1);
  CHECK(h.flush_count >= 1);
}

TEST_CASE("completed_fn is called even with pending fragments") {
  sink_test_harness h;
  mistral::tui::tui_render_sink sink(
    h.make_post(), h.make_append(), h.make_flush(),
    h.make_completed());

  sink.output("a");
  sink.output("b");
  sink.completed();

  h.run_all_posted();

  CHECK(h.appended.size() >= 2);
  CHECK(h.completed_count == 1);
}

TEST_CASE("completed_fn not called without completed() invocation") {
  sink_test_harness h;
  mistral::tui::tui_render_sink sink(
    h.make_post(), h.make_append(), h.make_flush(),
    h.make_completed());

  sink.output("hello");
  h.run_all_posted();

  CHECK(h.completed_count == 0);
}

TEST_CASE("completed_fn called once per completed() call") {
  sink_test_harness h;
  mistral::tui::tui_render_sink sink(
    h.make_post(), h.make_append(), h.make_flush(),
    h.make_completed());

  sink.output("a");
  sink.completed();
  h.run_all_posted();

  sink.output("b");
  sink.completed();
  h.run_all_posted();

  CHECK(h.completed_count == 2);
}

TEST_SUITE_END();