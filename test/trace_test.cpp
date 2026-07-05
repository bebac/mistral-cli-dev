#include "doctest/doctest.h"
#include "trace/trace.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <fstream>
#include <regex>

namespace mistral::trace {

// Test fixture for trace tests
struct TraceTestFixture {
    TraceTestFixture() {
        // Disable logger by default for tests
        logger::instance().disable();
    }

    ~TraceTestFixture() {
        logger::instance().disable();
    }
};

} // namespace mistral::trace

TEST_SUITE_BEGIN("mistral::trace");

// ============================================================================
// std::source_location validation tests
// ============================================================================

TEST_CASE("std::source_location is available and works") {
    auto loc = std::source_location::current();

    // Check that we can access the members
    CHECK(loc.file_name() != nullptr);
    CHECK(loc.line() > 0);
    CHECK(loc.function_name() != nullptr);

    // The file should contain "trace_test.cpp"
    std::string file = loc.file_name();
    CHECK(file.find("trace_test.cpp") != std::string::npos);
}

// ============================================================================
// Custom formatter for std::span<const std::byte> tests
// ============================================================================

TEST_CASE("custom formatter for std::span<const std::byte>") {
    using namespace std;

    const std::byte data[] = {std::byte{0x48}, std::byte{0x65}, std::byte{0x6c}, std::byte{0x6c}, std::byte{0x6f}};
    std::span<const std::byte> span(data);

    std::string result = std::format("bytes: {}", span);
    CHECK_EQ(result, "bytes: [48, 65, 6c, 6c, 6f]");
}

TEST_CASE("custom formatter for empty span") {
    using namespace std;

    std::span<const std::byte> empty;
    std::string result = std::format("bytes: {}", empty);
    CHECK_EQ(result, "bytes: []");
}

TEST_CASE("custom formatter for single byte span") {
    using namespace std;

    const std::byte data[] = {std::byte{0x41}};
    std::span<const std::byte> span(data);

    std::string result = std::format("bytes: {}", span);
    CHECK_EQ(result, "bytes: [41]");
}

// ============================================================================
// logger basic functionality tests
// ============================================================================

TEST_CASE_FIXTURE(mistral::trace::TraceTestFixture, "logger disabled by default") {
    using namespace mistral::trace;

    logger::instance().disable();
    CHECK_FALSE(logger::instance().enabled());
}

TEST_CASE_FIXTURE(mistral::trace::TraceTestFixture, "logger enable/disable") {
    using namespace mistral::trace;

    logger::instance().enable(true);
    CHECK(logger::instance().enabled());

    logger::instance().disable();
    CHECK_FALSE(logger::instance().enabled());

    logger::instance().enable();
    CHECK(logger::instance().enabled());
}

TEST_CASE_FIXTURE(mistral::trace::TraceTestFixture, "logger set_log_path") {
    using namespace mistral::trace;

    logger::instance().set_log_path("/tmp/test_trace.log");
}

TEST_CASE_FIXTURE(mistral::trace::TraceTestFixture, "logger category_to_string") {
    using namespace mistral::trace;

    logger& l = logger::instance();

    CHECK_EQ(l.category_to_string(category::connection), "connection");
    CHECK_EQ(l.category_to_string(category::stream), "stream");
    CHECK_EQ(l.category_to_string(category::flow_control), "flow_control");
    CHECK_EQ(l.category_to_string(category::data), "data");
    CHECK_EQ(l.category_to_string(category::sse), "sse");
    CHECK_EQ(l.category_to_string(category::parse), "parse");
    CHECK_EQ(l.category_to_string(category::policy), "policy");
    CHECK_EQ(l.category_to_string(category::approval), "approval");
}

TEST_CASE_FIXTURE(mistral::trace::TraceTestFixture, "logger timestamp format") {
    using namespace mistral::trace;

    logger& l = logger::instance();
    std::string ts = l.timestamp();

    // Should be ISO 8601 format with microseconds and Z
    CHECK(ts.size() >= 20);
    CHECK(ts.find('T') != std::string::npos);
    CHECK(ts.back() == 'Z');
    CHECK(ts.find('.') != std::string::npos);
}

// ============================================================================
// logger trace with formatting tests
// ============================================================================

TEST_CASE_FIXTURE(mistral::trace::TraceTestFixture, "logger trace with formatting") {
    using namespace mistral::trace;

    logger::instance().set_log_path("/tmp/test_trace_formatting.log");
    logger::instance().enable();

    // Test basic formatting
    logger::instance().trace(category::connection, "connecting to {}:{}", "api.mistral.ai", 443);
    logger::instance().trace(category::stream, "stream id={}", 1);
    logger::instance().trace(category::flow_control, "window={}", 1000);

    logger::instance().disable();

    // Check that the file was created and has content
    std::ifstream file("/tmp/test_trace_formatting.log");
    CHECK(file.is_open());
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    CHECK_FALSE(content.empty());
    CHECK(content.find("connecting to api.mistral.ai:443") != std::string::npos);
    CHECK(content.find("stream id=1") != std::string::npos);
    CHECK(content.find("window=1000") != std::string::npos);
}

TEST_CASE_FIXTURE(mistral::trace::TraceTestFixture, "logger with span formatting") {
    using namespace mistral::trace;

    logger::instance().set_log_path("/tmp/test_trace_span.log");
    logger::instance().enable();

    const std::byte data[] = {std::byte{0x48}, std::byte{0x65}, std::byte{0x6c}, std::byte{0x6c}, std::byte{0x6f}};
    std::span<const std::byte> span(data);

    logger::instance().trace(category::data, "received data: {}", span);

    logger::instance().disable();

    // Check that the file was created and has content
    std::ifstream file("/tmp/test_trace_span.log");
    CHECK(file.is_open());
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    CHECK_FALSE(content.empty());
    CHECK(content.find("received data: [48, 65, 6c, 6c, 6f]") != std::string::npos);
}

TEST_CASE_FIXTURE(mistral::trace::TraceTestFixture, "logger disabled has no overhead") {
    using namespace mistral::trace;

    logger::instance().disable();

    // Call trace many times - should be fast since it's just a bool check
    for (int i = 0; i < 10000; ++i) {
        logger::instance().trace(category::data, "test message {}", i);
    }

    CHECK(true);
}

TEST_CASE_FIXTURE(mistral::trace::TraceTestFixture, "source_location is captured correctly") {
    using namespace mistral::trace;

    constexpr char trace_filename[] = "/tmp/test_trace_source_location.log";

    std::remove(trace_filename);
    logger::instance().set_log_path(trace_filename);
    logger::instance().enable();

    std::source_location loc = std::source_location::current();
    logger::instance().trace(category::connection, "test connection");

    logger::instance().disable();

    // Check that the file contains source location info
    std::ifstream file(trace_filename);
    CHECK(file.is_open());
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    CHECK_FALSE(content.empty());

    // Should contain file, line, and function
    std::regex file_regex("\"file\":\\s*\"([^\"]*trace_test\\.cpp[^\"]*)\"");
    std::smatch file_match;
    CHECK(std::regex_search(content, file_match, file_regex));
    CHECK(file_match[1].str().find("trace_test.cpp") != std::string::npos);

    std::regex line_regex("\"line\":\\s*(\\d+)");
    std::smatch line_match;
    CHECK(std::regex_search(content, line_match, line_regex));
    CHECK(line_match[1] == std::to_string(loc.line()+1));

    CHECK(content.find("\"function\":") != std::string::npos);

    // Should contain the message
    CHECK(content.find("test connection") != std::string::npos);
}

TEST_SUITE_END();
