#include "connection.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

// ---- Lifecycle ----

void Connection::init(int fd, SSL* ssl, std::string root_dir,
                      int ka_timeout) noexcept
{
    fd_ = fd;
    ssl_ = ssl;
    root_dir_ = std::move(root_dir);
    keep_alive_timeout_ = ka_timeout;
    state_ = ssl ? State::TLS_ACCEPT : State::READING;
    last_active_ = Clock::now();
    headers_complete_ = false;
    header_sent_ = 0;
    file_fd_ = -1;
    file_size_ = 0;
    send_offset_ = 0;
    send_remaining_ = 0;
    chunk_used_ = 0;
    chunk_sent_ = 0;
    is_head_ = false;
}

void Connection::reset() noexcept
{
    request_.reset();
    response_.reset();
    header_buf_.clear();
    header_sent_ = 0;
    if (file_fd_ >= 0) {
        ::close(file_fd_);
        file_fd_ = -1;
    }
    file_size_ = 0;
    send_offset_ = 0;
    send_remaining_ = 0;
    chunk_used_ = 0;
    chunk_sent_ = 0;
    is_head_ = false;

    // Keep read_buf_ for pipelining (may have next request already)
}

void Connection::close() noexcept
{
    if (state_ == State::CLOSED) return;

    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }

    if (ssl_) {
        SSL_shutdown(ssl_); // best-effort, non-blocking
        SSL_free(ssl_);
        ssl_ = nullptr;
    }

    if (file_fd_ >= 0) {
        ::close(file_fd_);
        file_fd_ = -1;
    }

    state_ = State::CLOSED;
}

// ---- TLS Accept ----

Connection::Want Connection::do_tls_accept() noexcept
{
    int ret = SSL_accept(ssl_);
    if (ret == 1) {
        state_ = State::READING;
        last_active_ = Clock::now();
        return Want::READ;
    }

    int err = SSL_get_error(ssl_, ret);
    if (err == SSL_ERROR_WANT_READ)
        return Want::READ;
    if (err == SSL_ERROR_WANT_WRITE)
        return Want::WRITE;

    // Fatal TLS error
    response_.set_status(HttpResponse::Status::INTERNAL_ERROR);
    response_.set_body(HttpResponse::error_body(HttpResponse::Status::INTERNAL_ERROR));
    response_.set_keep_alive(false);
    response_.set_content_type("text/html; charset=utf-8");
    response_.set_content_length(response_.body().size());
    prepare_headers();
    state_ = State::SENDING_HEADERS;
    return Want::WRITE;
}

// ---- Read ----

Connection::Want Connection::do_read() noexcept
{
    last_active_ = Clock::now();

    size_t old_size = read_buf_.size();
    read_buf_.resize(old_size + 4096);

    ssize_t n;
    if (ssl_) {
        ERR_clear_error();
        n = SSL_read(ssl_, read_buf_.data() + old_size, 4096);
        if (n <= 0) {
            int err = SSL_get_error(ssl_, static_cast<int>(n));
            read_buf_.resize(old_size);
            if (err == SSL_ERROR_WANT_READ)
                return Want::READ;
            if (err == SSL_ERROR_WANT_WRITE)
                return Want::WRITE;
            // Connection closed or error
            close();
            return Want::NONE;
        }
    } else {
        n = ::read(fd_, read_buf_.data() + old_size, 4096);
        if (n < 0) {
            read_buf_.resize(old_size);
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return Want::READ;
            close();
            return Want::NONE;
        }
        if (n == 0) {
            // Peer closed
            close();
            return Want::NONE;
        }
    }

    read_buf_.resize(old_size + static_cast<size_t>(n));

    // Check for complete headers
    std::string_view buf(read_buf_.data(), read_buf_.size());
    if (buf.find("\r\n\r\n") != std::string_view::npos) {
        headers_complete_ = true;
        state_ = State::PARSING;
        return Want::NONE;
    }

    // Limit header size
    if (read_buf_.size() > 65536) {
        // Too large — close
        close();
        return Want::NONE;
    }

    return Want::READ;
}

// ---- Parse ----

Connection::Want Connection::do_parse() noexcept
{
    std::string_view buf(read_buf_.data(), read_buf_.size());
    if (!request_.parse(buf)) {
        response_.set_status(HttpResponse::Status::BAD_REQUEST);
        response_.set_body(HttpResponse::error_body(HttpResponse::Status::BAD_REQUEST));
        response_.set_keep_alive(false);
        response_.set_content_type("text/html; charset=utf-8");
        response_.set_content_length(response_.body().size());
        prepare_headers();
        state_ = State::SENDING_HEADERS;
        return Want::WRITE;
    }

    // Parse successful — event loop will call prepare_response
    return Want::NONE;
}

// ---- Send Headers ----

void Connection::prepare_headers() noexcept
{
    header_buf_ = response_.build_headers();
    header_sent_ = 0;
}

Connection::Want Connection::do_send_headers() noexcept
{
    auto remaining = header_buf_.size() - header_sent_;
    auto ptr = header_buf_.data() + header_sent_;

    ssize_t n;
    if (ssl_) {
        ERR_clear_error();
        n = SSL_write(ssl_, ptr, static_cast<int>(remaining));
        if (n <= 0) {
            int err = SSL_get_error(ssl_, static_cast<int>(n));
            if (err == SSL_ERROR_WANT_WRITE)
                return Want::WRITE;
            if (err == SSL_ERROR_WANT_READ)
                return Want::READ;
            close();
            return Want::NONE;
        }
    } else {
        n = ::write(fd_, ptr, remaining);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return Want::WRITE;
            close();
            return Want::NONE;
        }
    }

    header_sent_ += static_cast<size_t>(n);

    if (header_sent_ >= header_buf_.size()) {
        // Headers done
        if (is_head_ || !response_.has_body()) {
            // HEAD or no body → done
            if (response_.status() == HttpResponse::Status::NOT_MODIFIED) {
                // 304 has no body
            }
            state_ = State::KEEP_ALIVE;
            return Want::READ;
        }
        state_ = State::SENDING_BODY;
        // Try to send first body chunk immediately
        return do_send_body();
    }

    return Want::WRITE;
}

// ---- Send Body ----

void Connection::set_send_file(int fd, off_t size, off_t start,
                               off_t end, bool is_head) noexcept
{
    file_fd_ = fd;
    file_size_ = size;
    send_offset_ = start;
    send_remaining_ = (end >= start) ? (end - start + 1) : 0;
    is_head_ = is_head;
}

Connection::Want Connection::do_send_body_plain() noexcept
{
    if (send_remaining_ <= 0) {
        state_ = State::KEEP_ALIVE;
        if (file_fd_ >= 0) { ::close(file_fd_); file_fd_ = -1; }
        return Want::READ;
    }

    off_t remaining = send_remaining_;
    int ret = ::sendfile(file_fd_, fd_, send_offset_, &remaining, nullptr, 0);

    if (ret == 0) {
        // All bytes sent
        send_remaining_ = 0;
        state_ = State::KEEP_ALIVE;
        if (file_fd_ >= 0) { ::close(file_fd_); file_fd_ = -1; }
        return Want::READ;
    }

    if (ret == -1 && errno == EAGAIN) {
        send_offset_ += remaining;
        send_remaining_ -= remaining;
        return Want::WRITE;
    }

    // Error
    close();
    return Want::NONE;
}

Connection::Want Connection::do_send_body_tls() noexcept
{
    constexpr size_t CHUNK_SIZE = 65536;

    // If current chunk is fully sent, load next chunk
    if (chunk_sent_ >= chunk_used_) {
        if (send_remaining_ <= 0) {
            state_ = State::KEEP_ALIVE;
            if (file_fd_ >= 0) { ::close(file_fd_); file_fd_ = -1; }
            return Want::READ;
        }

        size_t to_read = static_cast<size_t>(send_remaining_);
        if (to_read > CHUNK_SIZE) to_read = CHUNK_SIZE;

        chunk_buf_.resize(to_read);
        auto n = ::pread(file_fd_, chunk_buf_.data(), to_read, send_offset_);
        if (n <= 0) {
            close();
            return Want::NONE;
        }
        chunk_used_ = static_cast<size_t>(n);
        chunk_sent_ = 0;
    }

    auto remaining = chunk_used_ - chunk_sent_;
    auto ptr = chunk_buf_.data() + chunk_sent_;

    ERR_clear_error();
    int n = SSL_write(ssl_, ptr, static_cast<int>(remaining));
    if (n <= 0) {
        int err = SSL_get_error(ssl_, n);
        if (err == SSL_ERROR_WANT_WRITE)
            return Want::WRITE;
        if (err == SSL_ERROR_WANT_READ)
            return Want::READ;
        close();
        return Want::NONE;
    }

    chunk_sent_ += static_cast<size_t>(n);

    if (chunk_sent_ >= chunk_used_) {
        // This chunk fully sent
        send_offset_ += static_cast<off_t>(chunk_used_);
        send_remaining_ -= static_cast<off_t>(chunk_used_);
    }

    return Want::WRITE;
}

Connection::Want Connection::do_send_body() noexcept
{
    if (ssl_)
        return do_send_body_tls();
    return do_send_body_plain();
}

// ---- Keep-Alive ----

bool Connection::is_expired(Clock::duration timeout) const noexcept
{
    return (Clock::now() - last_active_) > timeout;
}
