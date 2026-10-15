#include "mcp/http1_client.h"

#include "http/tls_connection.h"  // net::address_info resolver

#include <cerrno>
#include <cctype>
#include <cstring>
#include <stdexcept>

namespace mistral::mcp
{
  namespace
  {
    std::string_view trim(std::string_view s)
    {
      while (!s.empty() &&
             (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
      }
      while (!s.empty() &&
             (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
      }
      return s;
    }

    bool iequals(std::string_view a, std::string_view b)
    {
      if (a.size() != b.size()) return false;
      for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
          return false;
        }
      }
      return true;
    }

    std::string find_header(const http::headers& h, std::string_view name)
    {
      for (const auto& hdr : h.get()) {
        if (iequals(hdr.name, name)) {
          return hdr.value;
        }
      }
      return {};
    }
  } // namespace

  http1_client::~http1_client()
  {
    close();
  }

  // ---- Connection ----

  void http1_client::connect(const std::string& host, uint16_t port)
  {
    close();

    auto addr = net::address_info::resolve(host, std::to_string(port));
    if (!addr) {
      throw std::runtime_error(
        "failed to resolve " + host + ":" + std::to_string(port));
    }

    std::string last_error = "no addresses";
    for (auto* res : *addr) {
      try {
        dripcore::socket sock{res->ai_family, res->ai_socktype,
                              res->ai_protocol};
        sock.connect(res->ai_addr, res->ai_addrlen);
        sock.nonblocking(true);
        socket_ = std::move(sock);
        return;
      } catch (const std::exception& e) {
        last_error = e.what();
      }
    }
    throw std::runtime_error("connect to " + host + ":" +
      std::to_string(port) + " failed: " + last_error);
  }

  bool http1_client::connected() const
  {
    return socket_.has_value();
  }

  void http1_client::close()
  {
    socket_.reset();
    state_ = parse_state::idle;
    status_code_ = 0;
    response_headers_ = {};
    body_remaining_ = 0;
    out_buf_.clear();
    out_offset_ = 0;
    in_buf_.clear();
    events_.clear();
  }

  int http1_client::platform_handle() const
  {
    return socket_ ? socket_->platform_handle() : -1;
  }

  // ---- Request (write side) ----

  void http1_client::send_request(
    std::string_view method,
    std::string_view path,
    const http::headers& extra_headers,
    std::span<const std::byte> body)
  {
    // Reset response-side state for the new exchange.
    state_ = parse_state::status_line;
    status_code_ = 0;
    response_headers_ = {};
    body_remaining_ = 0;
    in_buf_.clear();
    events_.clear();

    std::string head;
    head.reserve(256);
    head.append(method).append(" ").append(path).append(" HTTP/1.1\r\n");
    for (const auto& h : extra_headers.get()) {
      head.append(h.name).append(": ").append(h.value).append("\r\n");
    }
    head.append("Content-Length: ")
      .append(std::to_string(body.size()))
      .append("\r\n");
    head.append("Connection: close\r\n\r\n");

    out_buf_.resize(head.size() + body.size());
    out_offset_ = 0;
    std::memcpy(out_buf_.data(), head.data(), head.size());
    if (!body.empty()) {
      std::memcpy(out_buf_.data() + head.size(), body.data(), body.size());
    }
  }

  std::span<const std::byte> http1_client::output_begin() const
  {
    return std::span<const std::byte>(
      out_buf_.data() + out_offset_, out_buf_.size() - out_offset_);
  }

  void http1_client::output_end(size_t n)
  {
    out_offset_ += n;
    if (out_offset_ >= out_buf_.size()) {
      out_buf_.clear();
      out_offset_ = 0;
    }
  }

  // ---- Response (read side) ----

  std::span<std::byte> http1_client::input_begin()
  {
    return std::span<std::byte>(read_buf_.data(), read_buf_.size());
  }

  void http1_client::input_end(size_t n)
  {
    in_buf_.insert(
      in_buf_.end(), read_buf_.data(), read_buf_.data() + n);
    parse_available();
  }

  void http1_client::eof()
  {
    if (state_ == parse_state::body_until_close) {
      events_.push_back(http1_event{
        http1_event_type::message_complete, status_code_, {}, {}});
      state_ = parse_state::complete;
    } else if (state_ != parse_state::complete) {
      events_.push_back(http1_event{
        http1_event_type::connection_closed, status_code_, {}, {}});
      state_ = parse_state::complete;
    }
  }

  // ---- Events ----

  bool http1_client::has_events() const
  {
    return !events_.empty();
  }

  std::optional<http1_event> http1_client::next_event()
  {
    if (events_.empty()) {
      return std::nullopt;
    }
    http1_event evt = std::move(events_.front());
    events_.erase(events_.begin());
    return evt;
  }

  // ---- Raw socket I/O ----

  int http1_client::read(std::span<std::byte> in)
  {
    if (!socket_) return -1;
    return static_cast<int>(socket_->recv(in.data(), in.size(), 0));
  }

  int http1_client::write(std::span<const std::byte> out)
  {
    if (!socket_ || out.empty()) return 0;
    return static_cast<int>(socket_->send(out.data(), out.size(), 0));
  }

  // ---- Response parser ----

  std::optional<std::string> http1_client::take_line()
  {
    for (size_t i = 0; i < in_buf_.size(); ++i) {
      if (in_buf_[i] == std::byte{'\n'}) {
        std::string line(
          reinterpret_cast<const char*>(in_buf_.data()), i);
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        in_buf_.erase(in_buf_.begin(), in_buf_.begin() + i + 1);
        return line;
      }
    }
    return std::nullopt;
  }

  void http1_client::emit_data(size_t n)
  {
    events_.push_back(http1_event{
      http1_event_type::data, status_code_, {},
      std::vector<std::byte>(in_buf_.begin(), in_buf_.begin() + n)});
    in_buf_.erase(in_buf_.begin(), in_buf_.begin() + n);
  }

  void http1_client::parse_available()
  {
    while (true) {
      switch (state_) {
        case parse_state::idle:
        case parse_state::complete:
          return;

        case parse_state::status_line: {
          auto line = take_line();
          if (!line) return;
          const std::string& l = *line;
          // "HTTP/1.1 200 OK"
          auto sp1 = l.find(' ');
          if (sp1 == std::string::npos) {
            events_.push_back(http1_event{
              http1_event_type::connection_closed, 0, {}, {}});
            state_ = parse_state::complete;
            return;
          }
          try {
            auto sp2 = l.find(' ', sp1 + 1);
            status_code_ = (sp2 == std::string::npos)
              ? std::stoi(l.substr(sp1 + 1))
              : std::stoi(l.substr(sp1 + 1, sp2 - sp1 - 1));
          } catch (const std::exception&) {
            events_.push_back(http1_event{
              http1_event_type::connection_closed, 0, {}, {}});
            state_ = parse_state::complete;
            return;
          }
          state_ = parse_state::headers;
          break;
        }

        case parse_state::headers: {
          auto line = take_line();
          if (!line) return;
          const std::string& l = *line;
          if (l.empty()) {
            // End of header block.
            events_.push_back(http1_event{http1_event_type::response_headers,
              status_code_, response_headers_, {}});

            std::string te =
              find_header(response_headers_, "transfer-encoding");
            std::string cl = find_header(response_headers_, "content-length");
            if (te.find("chunked") != std::string::npos) {
              state_ = parse_state::chunk_size;
            } else if (!cl.empty()) {
              body_remaining_ = std::stoull(cl);
              if (body_remaining_ == 0) {
                events_.push_back(http1_event{
                  http1_event_type::message_complete, status_code_, {}, {}});
                state_ = parse_state::complete;
              } else {
                state_ = parse_state::body_length;
              }
            } else {
              state_ = parse_state::body_until_close;
            }
            break;
          }
          auto colon = l.find(':');
          if (colon != std::string::npos) {
            response_headers_.add(
              std::string(trim(l.substr(0, colon))),
              std::string(trim(l.substr(colon + 1))));
          }
          break;
        }

        case parse_state::body_length: {
          if (in_buf_.empty()) return;
          size_t take = std::min(in_buf_.size(), body_remaining_);
          emit_data(take);
          body_remaining_ -= take;
          if (body_remaining_ == 0) {
            events_.push_back(http1_event{
              http1_event_type::message_complete, status_code_, {}, {}});
            state_ = parse_state::complete;
          }
          break;
        }

        case parse_state::chunk_size: {
          auto line = take_line();
          if (!line) return;
          const std::string& l = *line;
          // Hex size, possibly with chunk extensions after ';'.
          auto semi = l.find(';');
          std::string hex = (semi == std::string::npos)
            ? l : l.substr(0, semi);
          body_remaining_ = std::stoull(std::string(trim(hex)), nullptr, 16);
          state_ = (body_remaining_ == 0)
            ? parse_state::chunk_trailer
            : parse_state::chunk_data;
          break;
        }

        case parse_state::chunk_data: {
          if (in_buf_.empty()) return;
          size_t take = std::min(in_buf_.size(), body_remaining_);
          emit_data(take);
          body_remaining_ -= take;
          if (body_remaining_ == 0) {
            state_ = parse_state::chunk_crlf;
          }
          break;
        }

        case parse_state::chunk_crlf: {
          auto line = take_line();
          if (!line) return;
          state_ = parse_state::chunk_size;
          break;
        }

        case parse_state::chunk_trailer: {
          auto line = take_line();
          if (!line) return;
          const std::string& l = *line;
          if (l.empty()) {
            events_.push_back(http1_event{
              http1_event_type::message_complete, status_code_, {}, {}});
            state_ = parse_state::complete;
          }
          // else: trailer header line, ignored.
          break;
        }

        case parse_state::body_until_close: {
          if (in_buf_.empty()) return;
          emit_data(in_buf_.size());
          break;
        }
      }
    }
  }

} // namespace mistral::mcp
