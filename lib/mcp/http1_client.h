#ifndef MISTRAL_MCP_HTTP1_CLIENT_H
#define MISTRAL_MCP_HTTP1_CLIENT_H

#include <http/headers.h>

#include <dripcore/socket.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mistral::mcp
{
  // ---- Response events (mirrors http::event conceptually) ----

  enum class http1_event_type
  {
    response_headers,  // status line + headers parsed
    data,              // decoded body bytes
    message_complete,  // full response received
    connection_closed  // EOF before the response completed
  };

  struct http1_event
  {
    http1_event_type type;
    int status_code = 0;
    http::headers headers;         // response_headers
    std::vector<std::byte> data;   // data
  };

  // Minimal HTTP/1.1 client for the MCP Streamable HTTP transport.
  //
  // The API deliberately mirrors how stunning-waddle's client is driven
  // in this codebase (lib/http/http_client.h, http_read_task.h,
  // http_write_task.h), so it can be swapped for stunning-waddle with
  // minimal effort once it gains HTTP/1.1 support:
  //
  //   stunning-waddle                       http1_client
  //   ------------------------------------  ---------------------------
  //   http_client::connect()                connect()
  //   http_client::platform_handle()        platform_handle()
  //   http_client::read()/write()           read()/write()
  //   stream::send_headers/send_data/      send_request()
  //     send_end
  //   context::output_begin()/output_end()  output_begin()/output_end()
  //   context::input_begin()/input_end()    input_begin()/input_end()
  //   context::has_events()/next_event()    has_events()/next_event()
  //   http::event (headers/data/closed)     http1_event
  //
  // Differences from stunning-waddle, by design:
  //   - No multiplexing: one in-flight request per connection. Requests
  //     are sent with "Connection: close"; the caller reconnects for the
  //     next request.
  //   - The response parser handles Content-Length, chunked, and
  //     close-delimited bodies (SSE responses from the Storybook dev
  //     server arrive chunked or close-delimited).
  class http1_client
  {
  public:
    http1_client() = default;
    ~http1_client();

    http1_client(const http1_client&) = delete;
    http1_client& operator=(const http1_client&) = delete;

    // ---- Connection ----

    // Blocking connect (like net::tls_connection), then switches the
    // socket to non-blocking for the dripcore event loop. Throws
    // std::runtime_error on failure.
    void connect(const std::string& host, uint16_t port);
    bool connected() const;
    void close();

    // For dripcore readable()/writable() awaiters.
    int platform_handle() const;

    // ---- Request (write side) ----

    // Serializes a complete request into the pending output buffer.
    // Adds Content-Length and Connection: close automatically. Also
    // resets the response-side parser state for the new exchange.
    void send_request(
      std::string_view method,
      std::string_view path,
      const http::headers& extra_headers,
      std::span<const std::byte> body);

    // Bytes waiting to be written to the socket.
    std::span<const std::byte> output_begin() const;
    // Notifies that n bytes of the output buffer were written.
    void output_end(size_t n);

    // ---- Response (read side) ----

    // Read buffer for socket reads.
    std::span<std::byte> input_begin();
    // Feeds the first n bytes of the read buffer to the response
    // parser; produces events (drain with has_events/next_event).
    void input_end(size_t n);

    // Signals EOF. For close-delimited bodies with headers already
    // received this emits message_complete; otherwise
    // connection_closed.
    void eof();

    // ---- Events ----

    bool has_events() const;
    std::optional<http1_event> next_event();

    // ---- Raw socket I/O (mirrors http_client::read/write) ----

    // Returns bytes read (0 = EOF) or -1 (check errno; EAGAIN means
    // nothing available).
    int read(std::span<std::byte> in);
    // Returns bytes written or -1 (check errno; EAGAIN means the
    // socket buffer is full).
    int write(std::span<const std::byte> out);

  private:
    enum class parse_state
    {
      idle,
      status_line,
      headers,
      body_length,
      chunk_size,
      chunk_data,
      chunk_crlf,
      chunk_trailer,
      body_until_close,
      complete
    };

    std::optional<std::string> take_line();
    void emit_data(size_t n);
    void parse_available();

    std::optional<dripcore::socket> socket_;
    std::vector<std::byte> out_buf_;
    size_t out_offset_ = 0;
    std::array<std::byte, 16384> read_buf_{};
    std::vector<std::byte> in_buf_;
    std::vector<http1_event> events_;
    parse_state state_ = parse_state::idle;
    int status_code_ = 0;
    http::headers response_headers_;
    size_t body_remaining_ = 0;
  };

} // namespace mistral::mcp

#endif // MISTRAL_MCP_HTTP1_CLIENT_H
