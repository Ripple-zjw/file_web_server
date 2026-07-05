#pragma once

#include "http/http_request.h"
#include "http/http_response.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <openssl/ssl.h>
#include <string>
#include <vector>

/**
 * @brief TCP 连接状态机，管理单个连接的完整生命周期。
 *
 * 连接按状态机流转，每个状态有对应的 do_*() 动作方法，
 * 返回 Want::{READ, WRITE, NONE} 告诉事件循环下一步关注什么事件。
 *
 * 新连接先进入 PROTO_DETECT 状态，通过首字节判定协议：
 * - 0x16（TLS ClientHello）→ 启用 TLS 并握手
 * - 其他（HTTP 请求）→ 直接走明文
 *
 * 支持 TLS（按需），非 TLS 下使用 sendfile 零拷贝发送文件体，
 * TLS 下回退到分块 pread + SSL_write 方案。
 */
class Connection {
public:
    /// 连接状态
    enum class State : uint8_t {
        CLOSED,           ///< 已关闭，fd 被释放
        PROTO_DETECT,     ///< 等待首字节以判定 TLS/HTTP 协议
        TLS_ACCEPT,       ///< 正在进行 TLS 握手
        READING,          ///< 等待/正在读取 HTTP 请求数据
        PARSING,          ///< 请求头已读完，待解析
        SENDING_HEADERS,  ///< 正在发送 HTTP 响应头
        SENDING_BODY,     ///< 正在发送响应体（文件内容或错误页面）
        KEEP_ALIVE,       ///< 本次请求完成，等待下一个请求或超时
    };

    /// 事件关注方向
    enum class Want : uint8_t {
        NONE,  ///< 不需要关注任何事件（连接可能正等待内部处理）
        READ,  ///< 关注 EVFILT_READ 事件
        WRITE, ///< 关注 EVFILT_WRITE 事件
    };

    using Clock = std::chrono::steady_clock;

    Connection() = default;

    /**
     * @brief 初始化（或重新初始化）连接，设置 fd、根目录和超时参数。
     *
     * 初始状态始终为 PROTO_DETECT（协议由首字节判定），
     * TLS 上下文由 EventLoop 在检测到 TLS ClientHello 后注入。
     *
     * @param fd          已 accept 的 socket 文件描述符
     * @param root_dir    文件服务根目录（用于 keep-alive 重置时的请求处理）
     * @param ka_timeout  Keep-Alive 超时秒数
     */
    void init(int fd, std::string root_dir, int ka_timeout) noexcept;

    /**
     * @brief 重置请求/响应数据，准备处理同一连接上的下一个请求。
     *
     * 清空响应状态、头部缓冲区、文件发送状态和读缓冲区。
     * 注意：当前未真正支持 pipelining。
     */
    void reset() noexcept;

    /**
     * @brief 关闭连接：shutdown 并 close socket，释放 SSL 对象，关闭待发送的文件。
     */
    void close() noexcept;

    // ---- 协议检测与 TLS ----

    /**
     * @brief 通过 recv(MSG_PEEK) 探测首字节，判定 TLS/HTTP 并转换状态。
     *
     * - 首字节 0x16（TLS Record）且 TLS 可用 → 状态转为 TLS_ACCEPT，事件循环后续注入 SSL
     * - 首字节 0x16 但 TLS 不可用 → 构建 400 错误响应
     * - 非 0x16 → 视为明文 HTTP，状态转为 READING
     *
     * @param tls_available 服务器是否配置了 TLS 证书
     * @return 下一步应关注的事件方向
     */
    Want do_proto_detect(bool tls_available) noexcept;

    /// 设置 TLS 可用标志（由 EventLoop 在 create_connection 后调用）
    void set_tls_available(bool v) noexcept { tls_available_ = v; }
    /// 注入 SSL 对象（EventLoop 检测到 TLS 后调用）
    void set_ssl(SSL* ssl) noexcept { ssl_ = ssl; }

    // ---- 状态机动作方法 ----

    /// TLS 握手：尝试完成 SSL_accept，返回应关注的事件方向
    Want do_tls_accept() noexcept;
    /// 读取 HTTP 数据到 read_buf_，检测到双 CRLF 后转入 PARSING 状态
    Want do_read()       noexcept;
    /// 解析 read_buf_ 中的 HTTP 请求，失败则构建 400 错误响应
    Want do_parse()      noexcept;
    /// 发送响应头字符串（逐字节发送，处理 EAGAIN 半写）
    Want do_send_headers() noexcept;
    /// 发送响应体（非 TLS 用 sendfile，TLS 用分块 pread + SSL_write）
    Want do_send_body()    noexcept;

    // ---- 响应准备 ----

    /// 将 HttpResponse 序列化为头部字符串并存储到 header_buf_，重置发送偏移
    void prepare_headers() noexcept;

    /**
     * @brief 设置待发送的文件信息。
     * @param fd      已打开的文件描述符（所有权转移给 Connection）
     * @param size    文件总大小
     * @param start   发送起始偏移（用于 Range）
     * @param end     发送结束偏移
     * @param is_head 是否仅发送头部（HEAD 方法）
     */
    void set_send_file(int fd, off_t size, off_t start, off_t end,
                       bool is_head) noexcept;

    /**
     * @brief 检查自上次活动以来是否已超过超时时间。
     * @param timeout 超时时间
     * @return true 表示连接已过期应被清理
     */
    bool is_expired(Clock::duration timeout) const noexcept;

    // ---- 访问器 ----

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
    bool    tls_available_ = false;    ///< 服务器是否支持 TLS（由 EventLoop 设置）

    // ---- 读缓冲区 ----
    std::vector<char> read_buf_;      ///< 累积已读取的原始 HTTP 字节
    bool headers_complete_ = false;   ///< 是否已检测到 \r\n\r\n 终止标记

    // ---- 头部发送 ----
    std::string header_buf_;          ///< 预构建的响应头字符串
    size_t header_sent_ = 0;          ///< 已发送的头部字节数（支持分次写）

    // ---- 文件发送状态 ----
    int    file_fd_ = -1;             ///< 待发送文件的描述符（发送完成后由 close/reset 关闭）
    off_t  file_size_ = 0;            ///< 文件总大小
    off_t  send_offset_ = 0;          ///< 当前发送位置（文件偏移）
    off_t  send_remaining_ = 0;       ///< 剩余待发送字节数

    // ---- TLS 分块缓冲区 ----
    std::vector<char> chunk_buf_;     ///< 从文件预读的当前分块数据
    size_t chunk_used_ = 0;           ///< chunk_buf_ 中有效数据的字节数
    size_t chunk_sent_ = 0;           ///< chunk_buf_ 中已通过 SSL_write 发送的字节数

    // ---- 内存 body 发送（错误页面等） ----
    size_t body_sent_ = 0;            ///< response_.body() 中已发送的字节数

    // ---- 请求/响应 ----
    HttpRequest  request_;
    HttpResponse response_;
    bool is_head_ = false;            ///< 当前请求是否为 HEAD 方法（不发送响应体）

    // ---- Keep-Alive ----
    Clock::time_point last_active_;   ///< 最后一次 I/O 活动的时间点
    int  keep_alive_timeout_ = 75;    ///< Keep-Alive 超时秒数
    std::string root_dir_;            ///< 文件服务根目录（keep-alive 重置时使用）

    /// 非 TLS 响应体发送：使用 sendfile 零拷贝
    Want do_send_body_plain() noexcept;
    /// TLS 响应体发送：pread 分块读文件 + SSL_write
    Want do_send_body_tls() noexcept;
};
