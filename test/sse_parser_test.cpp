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

TEST_CASE("sse_parser: multi-line data fields")
{
  mistral::api::sse_parser parser;

  // SSE allows data to span multiple lines
  // This should concatenate without adding extra newlines
  auto events = parser.feed("data: {\"first\":\"part\"}\ndata: \"second\"}\n\n");

  REQUIRE(events.size() == 1);
  CHECK(events[0].kind == mistral::api::sse_parser::event::type::data);

  // The payload should be concatenated without newlines
  // Current buggy implementation adds a '\n' between the parts
  CHECK(events[0].payload == "{\"first\":\"part\"}\"second\"}");
}

TEST_CASE("sse_parser: multi-line data with three chunks")
{
  mistral::api::sse_parser parser;

  auto events = parser.feed(
    "data: {\"a\":1}\n"
    "data: ,\"b\":2}\n"
    "data: ,\"c\":3}\n\n"
  );

  REQUIRE(events.size() == 1);
  CHECK(events[0].kind == mistral::api::sse_parser::event::type::data);

  // Should be valid JSON without newlines
  CHECK(events[0].payload == "{\"a\":1},\"b\":2},\"c\":3}");
}

TEST_CASE("sse_parser: chunked multi-line data across feeds")
{
  mistral::api::sse_parser parser;

  // First chunk with first part of data
  auto e1 = parser.feed("data: {\"multi");
  CHECK(e1.empty());  // Incomplete

  // Second chunk with continuation on new line
  auto e2 = parser.feed("\ndata: \"line\"}\n\n");
  REQUIRE(e2.size() == 1);
  CHECK(e2[0].kind == mistral::api::sse_parser::event::type::data);

  // Should be concatenated without newline: {"multi + "line"} = {"multi"line"}
  CHECK(e2[0].payload == "{\"multi\"line\"}");
}
