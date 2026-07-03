#include "connection.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

// ============================================================================
// 生命周期管理
// ============================================================================

/**
 * @brief 初始化连接 —— 设置 fd、TLS 对象、根目录和超时参数。
 *
 * 初始状态：提供 SSL 对象时进入 TLS_ACCEPT，否则进入 READING。
 * 重置所有内部计数器、缓冲区偏移和文件发送状态。
 *
 * @param fd          已 accept 的 socket fd
 * @param ssl         SSL 对象（nullptr 表示不启用 TLS）
 * @param root_dir    文件服务根目录
 * @param ka_timeout  Keep-Alive 超时秒数
 */
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

/**
 * @brief 重置连接以处理同一 socket 上的下一个请求。
 *
 * 清空请求/响应对象、头部发送缓冲区、文件发送状态、读缓冲区。
 * 注意：不重置 last_active_（它应由下次 I/O 自动更新）。
 */
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

    // 清空读缓冲区：当前未正确支持 pipelining，
    // 保留旧数据会导致内存膨胀和重复响应
    read_buf_.clear();
}

/**
 * @brief 彻底关闭连接 —— shutdown + close socket，释放 SSL 对象，关闭待发送文件。
 *
 * 幂等操作：state_ 已为 CLOSED 时直接返回。
 * SSL_shutdown 以 best-effort 方式执行（非阻塞，不接受返回值）。
 */
void Connection::close() noexcept
{
    if (state_ == State::CLOSED) return;

    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }

    if (ssl_) {
        SSL_shutdown(ssl_); // best-effort，不阻塞
        SSL_free(ssl_);
        ssl_ = nullptr;
    }

    if (file_fd_ >= 0) {
        ::close(file_fd_);
        file_fd_ = -1;
    }

    state_ = State::CLOSED;
}

// ============================================================================
// TLS 握手
// ============================================================================

/**
 * @brief 尝试完成 TLS 握手（SSL_accept）。
 *
 * 调用 SSL_accept，根据返回值：
 * - 1（成功）→ 转入 READING 状态，返回 Want::READ 等待 HTTP 数据
 * - SSL_ERROR_WANT_READ/WRITE → 返回对应 Want，让事件循环在下一次可读/写时重试
 * - 其他错误 → 构建 500 错误响应，转入 SENDING_HEADERS
 *
 * @return 下一步应关注的事件方向
 */
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

    // TLS 严重错误 → 发送 500 错误响应
    response_.set_status(HttpResponse::Status::INTERNAL_ERROR);
    response_.set_body(HttpResponse::error_body(HttpResponse::Status::INTERNAL_ERROR));
    response_.set_keep_alive(false);
    response_.set_content_type("text/html; charset=utf-8");
    response_.set_content_length(response_.body().size());
    prepare_headers();
    state_ = State::SENDING_HEADERS;
    return Want::WRITE;
}

// ============================================================================
// 数据读取
// ============================================================================

/**
 * @brief 从 socket/SSL 读取数据到 read_buf_。
 *
 * 每次尝试读取 4096 字节。检测到 \r\n\r\n 后将状态转为 PARSING 并
 * 返回 Want::NONE（由事件循环的 process_connection 在下一行内联调用 do_parse）。
 *
 * 安全限制：read_buf_ 最大 65536 字节，超出则关闭连接（防止内存攻击）。
 *
 * @return 下一步应关注的事件方向
 */
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
            // 连接关闭或错误
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
            // 对端已关闭连接
            close();
            return Want::NONE;
        }
    }

    read_buf_.resize(old_size + static_cast<size_t>(n));

    // 检查是否已接收到完整的 HTTP 头部终止标记
    std::string_view buf(read_buf_.data(), read_buf_.size());
    if (buf.find("\r\n\r\n") != std::string_view::npos) {
        headers_complete_ = true;
        state_ = State::PARSING;
        return Want::NONE;
    }

    // 头部大小超过 64KB → 关闭连接（拒绝超大请求头攻击）
    if (read_buf_.size() > 65536) {
        close();
        return Want::NONE;
    }

    return Want::READ;
}

// ============================================================================
// 请求解析
// ============================================================================

/**
 * @brief 调用 HttpRequest::parse 解析 read_buf_ 中的 HTTP 请求。
 *
 * 解析失败则构建 400 Bad Request 响应并转入 SENDING_HEADERS。
 * 成功则返回 Want::NONE，由事件循环调用 prepare_response 继续处理。
 *
 * @return 下一步应关注的事件方向
 */
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

    // 解析成功 —— 事件循环将调用 prepare_response 构建响应
    return Want::NONE;
}

// ============================================================================
// 响应头发送
// ============================================================================

/// 将 HttpResponse 序列化为头部字符串存储到 header_buf_，重置发送偏移。
void Connection::prepare_headers() noexcept
{
    header_buf_ = response_.build_headers();
    header_sent_ = 0;
}

/**
 * @brief 发送 HTTP 响应头（逐字节发送，支持非阻塞半写）。
 *
 * 发送完成后判断：
 * - HEAD 方法或无 body → 直接进入 KEEP_ALIVE
 * - 有 body → 转入 SENDING_BODY 并立即尝试发送首块数据
 *
 * @return 下一步应关注的事件方向
 */
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
        // 头部发送完成
        if (is_head_ || !response_.has_body()) {
            // HEAD 方法或没有 body → 当前请求完成
            state_ = State::KEEP_ALIVE;
            return Want::READ;
        }
        state_ = State::SENDING_BODY;
        // 尝试立即发送第一个 body 分块
        return do_send_body();
    }

    return Want::WRITE;
}

// ============================================================================
// 响应体发送
// ============================================================================

/**
 * @brief 设置待发送的文件信息。
 * @param fd      已打开的文件描述符（所有权转移给 Connection）
 * @param size    文件总大小（字节）
 * @param start   发送起始偏移（用于 Range 请求的起点）
 * @param end     发送结束偏移（用于 Range 请求的终点）
 * @param is_head 是否为 HEAD 请求（仅发送头部不发送体）
 */
void Connection::set_send_file(int fd, off_t size, off_t start,
                               off_t end, bool is_head) noexcept
{
    file_fd_ = fd;
    file_size_ = size;
    send_offset_ = start;
    send_remaining_ = (end >= start) ? (end - start + 1) : 0;
    is_head_ = is_head;
}

/**
 * @brief 非 TLS 路径的响应体发送 —— 使用 sendfile() 零拷贝发送文件内容。
 *
 * sendfile 可能只发送了部分数据（EAGAIN），此时系统已更新 remaining 参数
 * 为实际发送的字节数，我们据此更新 send_offset_ 和 send_remaining_。
 *
 * @return 下一步应关注的事件方向：
 *         - 全部发送完成返回 Want::READ（进入 KEEP_ALIVE）
 *         - 部分发送（EAGAIN）返回 Want::WRITE
 */
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
        // 全部字节已发送
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

    // 文件读取错误
    close();
    return Want::NONE;
}

/**
 * @brief TLS 路径的响应体发送 —— pread 分块读取文件 + SSL_write 发送。
 *
 * 由于 sendfile 不适用于 SSL 连接，这里使用两阶段循环：
 * 1. 从 file_fd_ 预读最多 65536 字节到 chunk_buf_
 * 2. 通过 SSL_write 逐次发送 chunk_buf_ 中的内容
 * 当一个 chunk 完全发送后，预读下一个块。
 *
 * @return 下一步应关注的事件方向：
 *         - 全部发送完成返回 Want::READ（进入 KEEP_ALIVE）
 *         - 当前块未发送完返回 Want::WRITE
 */
Connection::Want Connection::do_send_body_tls() noexcept
{
    constexpr size_t CHUNK_SIZE = 65536;

    // 当前 chunk 已全部发送 → 加载下一个 chunk
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
        // 当前 chunk 完全发送，更新文件偏移
        send_offset_ += static_cast<off_t>(chunk_used_);
        send_remaining_ -= static_cast<off_t>(chunk_used_);
    }

    return Want::WRITE;
}

/**
 * @brief 响应体发送入口 —— 根据是否 TLS 路由到对应实现。
 *
 * @return 下一步应关注的事件方向
 */
Connection::Want Connection::do_send_body() noexcept
{
    if (ssl_)
        return do_send_body_tls();
    return do_send_body_plain();
}

// ============================================================================
// Keep-Alive 超时检查
// ============================================================================

/**
 * @brief 检查自上次 I/O 活动以来是否已超过超时阈值。
 * @param timeout 超时时间
 * @return true 表示连接已过期
 */
bool Connection::is_expired(Clock::duration timeout) const noexcept
{
    return (Clock::now() - last_active_) > timeout;
}
