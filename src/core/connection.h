#pragma once

#include "http/http_request.h"
#include "http/http_response.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <openssl/ssl.h>
#include <string>
#include <vector>

class Connection {
public:
    enum class State : uint8_t {
        CLOSED,
        TLS_ACCEPT,
        READING,
        PARSING,
        SENDING_HEADERS,
        SENDING_BODY,
        KEEP_ALIVE,
    };

    enum class Want : uint8_t {
        NONE,
        READ,
        WRITE,
    };

    using Clock = std::chrono::steady_clock;

    Connection() = default;

    void init(int fd, SSL* ssl, std::string root_dir, int ka_timeout) noexcept;
    void reset() noexcept;
    void close() noexcept;

    // State machine actions — return what event to wait for next
    Want do_tls_accept() noexcept;
    Want do_read()       noexcept;
    Want do_parse()      noexcept;
    Want do_send_headers() noexcept;
    Want do_send_body()    noexcept;

    // Response preparation
    void prepare_headers() noexcept;
    void set_send_file(int fd, off_t size, off_t start, off_t end,
                       bool is_head) noexcept;

    bool is_expired(Clock::duration timeout) const noexcept;

    // Accessors
    int         fd()    const noexcept { return fd_; }
    State       state() const noexcept { return state_; }
    bool        is_closed() const noexcept { return state_ == State::CLOSED; }
    bool        is_tls()    const noexcept { return ssl_ != nullptr; }

    HttpRequest&       request()  noexcept { return request_; }
    const HttpRequest& request()  const noexcept { return request_; }
    HttpResponse&      response() noexcept { return response_; }

private:
    int     fd_ = -1;
    State   state_ = State::CLOSED;
    SSL*    ssl_ = nullptr;

    // Read buffer
    std::vector<char> read_buf_;
    bool headers_complete_ = false;

    // Header send
    std::string header_buf_;
    size_t header_sent_ = 0;

    // File send state
    int    file_fd_ = -1;
    off_t  file_size_ = 0;
    off_t  send_offset_ = 0;
    off_t  send_remaining_ = 0;

    // TLS body chunk buffer
    std::vector<char> chunk_buf_;
    size_t chunk_used_ = 0;
    size_t chunk_sent_ = 0;

    // Request/response
    HttpRequest  request_;
    HttpResponse response_;
    bool is_head_ = false;

    // Keep-alive
    Clock::time_point last_active_;
    int  keep_alive_timeout_ = 75;
    std::string root_dir_;

    // Non-TLS body send
    Want do_send_body_plain() noexcept;
    // TLS body send
    Want do_send_body_tls() noexcept;
};
