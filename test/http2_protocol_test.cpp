#include "doctest/doctest.h"

#include "http/client_context.h"
#include "http/error_codes.h"
#include "http/event.h"
#include "http/v2/frame.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
  // Sans-I/O helpers: feed raw bytes into the engine's input buffer and
  // capture the frames it queues for output. Mirrors the helpers in
  // stunning-waddle's own test suite.

  void feed(http::client_context& ctx, std::span<const std::byte> data)
  {
    while (!data.empty())
    {
      auto in = ctx.input_begin();
      REQUIRE(!in.empty());

      const size_t n = std::min(in.size(), data.size());
      std::memcpy(in.data(), data.data(), n);
      ctx.input_end(n);
      data = data.subspan(n);
    }
  }

  struct captured_frame
  {
    uint8_t type = 0;
    uint8_t flags = 0;
    uint32_t stream_id = 0;
    std::vector<std::byte> payload;
  };

  std::optional<captured_frame> capture(http::client_context& ctx)
  {
    auto out = ctx.output_begin();
    if (out.size() < 9)
    {
      return std::nullopt;
    }

    const auto byte_at = [&out](size_t i) { return std::to_integer<uint32_t>(out[i]); };

    const uint32_t length = (byte_at(0) << 16) | (byte_at(1) << 8) | byte_at(2);

    captured_frame frame;
    frame.type = static_cast<uint8_t>(byte_at(3));
    frame.flags = static_cast<uint8_t>(byte_at(4));
    frame.stream_id = ((byte_at(5) & 0x7f) << 24) | (byte_at(6) << 16) | (byte_at(7) << 8) | byte_at(8);

    if (out.size() < 9 + length)
    {
      return std::nullopt;
    }

    frame.payload.assign(out.begin() + 9, out.begin() + 9 + length);
    ctx.output_end(9 + length);
    return frame;
  }

  // Drop everything the engine wants to send (client preface + SETTINGS).
  void drain_output(http::client_context& ctx)
  {
    constexpr std::string_view preface{"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"};

    while (true)
    {
      auto out = ctx.output_begin();
      if (out.empty())
      {
        return;
      }

      // The connection preface is not a frame; skip it.
      if (out.size() >= preface.size() && std::memcmp(out.data(), preface.data(), preface.size()) == 0)
      {
        ctx.output_end(preface.size());
        continue;
      }

      if (!capture(ctx))
      {
        return;
      }
    }
  }

  std::vector<std::byte> make_frame(uint8_t type, uint8_t flags, uint32_t stream_id, std::span<const std::byte> payload)
  {
    std::vector<std::byte> frame;
    const uint32_t length = static_cast<uint32_t>(payload.size());

    frame.push_back(static_cast<std::byte>((length >> 16) & 0xff));
    frame.push_back(static_cast<std::byte>((length >> 8) & 0xff));
    frame.push_back(static_cast<std::byte>(length & 0xff));
    frame.push_back(static_cast<std::byte>(type));
    frame.push_back(static_cast<std::byte>(flags));
    frame.push_back(static_cast<std::byte>((stream_id >> 24) & 0x7f));
    frame.push_back(static_cast<std::byte>((stream_id >> 16) & 0xff));
    frame.push_back(static_cast<std::byte>((stream_id >> 8) & 0xff));
    frame.push_back(static_cast<std::byte>(stream_id & 0xff));
    frame.insert(frame.end(), payload.begin(), payload.end());

    return frame;
  }

  std::vector<std::byte> make_rst_stream(uint32_t stream_id, uint32_t wire_error_code)
  {
    std::array<std::byte, 4> payload{
      static_cast<std::byte>((wire_error_code >> 24) & 0xff),
      static_cast<std::byte>((wire_error_code >> 16) & 0xff),
      static_cast<std::byte>((wire_error_code >> 8) & 0xff),
      static_cast<std::byte>(wire_error_code & 0xff),
    };
    return make_frame(0x03, 0x00, stream_id, payload);
  }

  std::vector<std::byte> make_ping(std::span<const std::byte> payload, bool ack)
  {
    return make_frame(0x06, static_cast<uint8_t>(ack ? 0x01 : 0x00), 0, payload);
  }

  std::vector<std::byte> make_goaway(uint32_t last_stream_id, uint32_t wire_error_code)
  {
    std::array<std::byte, 8> payload{
      static_cast<std::byte>((last_stream_id >> 24) & 0x7f),
      static_cast<std::byte>((last_stream_id >> 16) & 0xff),
      static_cast<std::byte>((last_stream_id >> 8) & 0xff),
      static_cast<std::byte>(last_stream_id & 0xff),
      static_cast<std::byte>((wire_error_code >> 24) & 0xff),
      static_cast<std::byte>((wire_error_code >> 16) & 0xff),
      static_cast<std::byte>((wire_error_code >> 8) & 0xff),
      static_cast<std::byte>(wire_error_code & 0xff),
    };
    return make_frame(0x07, 0x00, 0, payload);
  }

  // A client context with the preface sent, the server's SETTINGS
  // acknowledged, and one open stream.
  struct client_fixture
  {
    http::client_context ctx;
    uint32_t stream_id = 0;

    client_fixture()
    {
      stream_id = ctx.open_stream().id();
      drain_output(ctx);

      // Empty SETTINGS from the server; the engine answers with a SETTINGS ACK.
      feed(ctx, make_frame(0x04, 0x00, 0, {}));
      drain_output(ctx);
    }
  };
} // namespace

TEST_CASE("http2: RST_STREAM surfaces the peer's error code")
{
  client_fixture f;

  feed(f.ctx, make_rst_stream(f.stream_id, 0x08)); // CANCEL

  REQUIRE(f.ctx.has_events());
  auto evt = f.ctx.next_event();
  REQUIRE(evt.has_value());

  CHECK(evt->type == http::event_type::stream_reset);
  CHECK(evt->stream_id == f.stream_id);
  CHECK(evt->http2_error_code == 0x08);

  auto ec = std::get<std::error_code>(evt->payload);
  CHECK(ec.value() == static_cast<int>(http::error_code::cancel));
  CHECK(ec.message() == "Cancelled");
}

TEST_CASE("http2: RST_STREAM split across reads is decoded exactly once")
{
  client_fixture f;

  auto frame = make_rst_stream(f.stream_id, 0x07); // REFUSED_STREAM
  const std::span<const std::byte> bytes(frame);

  // Everything except the last payload byte: the engine must wait for the
  // rest instead of decoding a truncated error code.
  feed(f.ctx, bytes.first(bytes.size() - 1));
  CHECK_FALSE(f.ctx.has_events());

  feed(f.ctx, bytes.subspan(bytes.size() - 1));

  REQUIRE(f.ctx.has_events());
  auto evt = f.ctx.next_event();
  REQUIRE(evt.has_value());
  CHECK(evt->http2_error_code == 0x07);
  CHECK(std::get<std::error_code>(evt->payload).value() == static_cast<int>(http::error_code::refused_stream));
  CHECK_FALSE(f.ctx.has_events());
}

TEST_CASE("http2: PING is answered with a matching PING ACK")
{
  client_fixture f;

  std::array<std::byte, 8> ping_payload{
    std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
    std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08},
  };

  feed(f.ctx, make_ping(ping_payload, false));

  auto ack = capture(f.ctx);
  REQUIRE(ack.has_value());
  CHECK(ack->type == 0x06);  // PING
  CHECK(ack->flags == 0x01); // ACK
  CHECK(ack->stream_id == 0);
  REQUIRE(ack->payload.size() == ping_payload.size());
  CHECK(std::memcmp(ack->payload.data(), ping_payload.data(), ping_payload.size()) == 0);

  // A PING must not produce protocol events.
  CHECK_FALSE(f.ctx.has_events());
}

TEST_CASE("http2: PING ACK is not answered")
{
  client_fixture f;

  std::array<std::byte, 8> ping_payload{};
  feed(f.ctx, make_ping(ping_payload, true));

  CHECK_FALSE(capture(f.ctx).has_value());
  CHECK_FALSE(f.ctx.has_events());
}

TEST_CASE("http2: GOAWAY maps the wire error code")
{
  client_fixture f;

  feed(f.ctx, make_goaway(f.stream_id, 0x0b)); // ENHANCE_YOUR_CALM

  REQUIRE(f.ctx.has_events());
  auto evt = f.ctx.next_event();
  REQUIRE(evt.has_value());

  CHECK(evt->type == http::event_type::goaway);
  CHECK(evt->http2_error_code == 0x0b);

  auto info = std::get<http::goaway_info>(evt->payload);
  CHECK(info.last_stream_id == f.stream_id);
  CHECK(static_cast<uint32_t>(info.ec) == static_cast<uint32_t>(http::error_code::enhance_your_calm));
}

TEST_CASE("http2: wire error code mapping round-trips")
{
  const std::pair<uint32_t, http::error_code> cases[] = {
    {0x00, http::error_code::no_error},
    {0x01, http::error_code::protocol_error},
    {0x02, http::error_code::internal_error},
    {0x03, http::error_code::flow_control_error},
    {0x04, http::error_code::settings_timeout},
    {0x05, http::error_code::stream_closed},
    {0x06, http::error_code::frame_size_error},
    {0x07, http::error_code::refused_stream},
    {0x08, http::error_code::cancel},
    {0x09, http::error_code::compression_error},
    {0x0a, http::error_code::connect_error},
    {0x0b, http::error_code::enhance_your_calm},
    {0x0c, http::error_code::inadequate_security},
    {0x0d, http::error_code::http_1_1_required},
  };

  for (const auto& [wire, expected] : cases)
  {
    CHECK(static_cast<uint32_t>(http::error_code_from_http2(wire)) == static_cast<uint32_t>(expected));
    CHECK(http::http2_wire_code(expected) == wire);
  }

  // Unknown wire codes are preserved verbatim rather than remapped.
  CHECK(static_cast<uint32_t>(http::error_code_from_http2(0x42)) == 0x42);
}
