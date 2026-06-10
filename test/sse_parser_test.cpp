#include "doctest/doctest.h"
#include "api/sse_parser.h"

TEST_CASE("sse_parser: single complete event")
{
  mistral::api::sse_parser parser;
  auto events = parser.feed("data: {\"test\":true}\n\n");

  REQUIRE(events.size() == 1);
  CHECK(events[0].kind == mistral::api::sse_parser::event::type::data);
  CHECK(events[0].payload == "{\"test\":true}");
}

TEST_CASE("sse_parser: [DONE] sentinel")
{
  mistral::api::sse_parser parser;
  auto events = parser.feed("data: [DONE]\n\n");

  REQUIRE(events.size() == 1);
  CHECK(events[0].kind == mistral::api::sse_parser::event::type::done);
}

TEST_CASE("sse_parser: multiple events in one chunk")
{
  mistral::api::sse_parser parser;
  auto events = parser.feed(
    "data: {\"a\":1}\n\n"
    "data: {\"b\":2}\n\n"
    "data: [DONE]\n\n"
  );

  REQUIRE(events.size() == 3);
  CHECK(events[0].payload == "{\"a\":1}");
  CHECK(events[1].payload == "{\"b\":2}");
  CHECK(events[2].kind == mistral::api::sse_parser::event::type::done);
}

TEST_CASE("sse_parser: chunked delivery across feeds")
{
  mistral::api::sse_parser parser;

  auto e1 = parser.feed("data: {\"par");
  CHECK(e1.empty());  // Incomplete line.

  auto e2 = parser.feed("tial\":true}\n\n");
  REQUIRE(e2.size() == 1);
  CHECK(e2[0].payload == "{\"partial\":true}");
}

TEST_CASE("sse_parser: handles \\r\\n line endings")
{
  mistral::api::sse_parser parser;
  auto events = parser.feed("data: {\"cr\":true}\r\n\r\n");

  REQUIRE(events.size() == 1);
  CHECK(events[0].payload == "{\"cr\":true}");
}

TEST_CASE("sse_parser: reset clears state")
{
  mistral::api::sse_parser parser;
  parser.feed("data: incomplete");
  parser.reset();

  auto events = parser.feed("data: fresh\n\n");
  REQUIRE(events.size() == 1);
  CHECK(events[0].payload == "fresh");
}
