#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * @brief HTTP 响应构建器，支持设置状态码、头部和正文，并将所有头部序列化为响应字符串。
 *
 * 由事件循环准备响应内容后，通过 build_headers() 生成完整的 HTTP 响应头文本。
 * 正文数据（文件内容）不在本类中处理，由 Connection 通过 sendfile/S SL_write 单独发送。
 */
class HttpResponse {
public:
    enum class Status : uint16_t {
        OK = 200,
        PARTIAL_CONTENT = 206,
        NOT_MODIFIED = 304,
        BAD_REQUEST = 400,
        FORBIDDEN = 403,
        NOT_FOUND = 404,
        METHOD_NOT_ALLOWED = 405,
        RANGE_NOT_SATISFIABLE = 416,
        INTERNAL_ERROR = 500,
        NOT_IMPLEMENTED = 501,
    };

    /// 设置 HTTP 状态码
    void set_status(Status s) noexcept { status_ = s; }
    /// 添加一个自定义头部
    void set_header(std::string name, std::string value) noexcept;
    /// 设置 Content-Type 头部
    void set_content_type(std::string_view ct) noexcept;
    /// 设置 Content-Length 头部
    void set_content_length(uint64_t len) noexcept;
    /// 设置 Last-Modified 头部（Unix 时间戳）
    void set_last_modified(std::time_t tm) noexcept;
    /// 设置 Accept-Ranges 头部
    void set_accept_ranges(bool v) noexcept { accept_ranges_ = v; }
    /// 设置 Connection 头部的行为（keep-alive 或 close）
    void set_keep_alive(bool v) noexcept { keep_alive_ = v; }
    /// 设置响应正文（用于错误页面等小文本响应）
    void set_body(std::string b) noexcept { body_ = std::move(b); }

    Status status() const noexcept { return status_; }
    bool   has_body() const noexcept { return !body_.empty(); }
    std::string_view body() const noexcept { return body_; }

    /**
     * @brief 构建完整的 HTTP 响应头字符串（含状态行、所有标准头部、自定义头部、终止空行）。
     * @return 可直接通过 socket/SSL 发送的字节序列
     */
    std::string build_headers() const noexcept;
    /// 重置所有字段为默认值
    void reset() noexcept;

    /**
     * @brief 将状态码转换为原因短语。
     * @param s HTTP 状态码
     * @return 如 "OK"、"Not Found" 等
     */
    static const char* reason(Status s) noexcept;
    /**
     * @brief 生成包含状态码和原因的 HTML 错误页面。
     * @param s HTTP 状态码
     * @return 完整的 HTML 错误页面字符串
     */
    static std::string error_body(Status s) noexcept;

private:
    Status status_ = Status::OK;
    std::vector<std::pair<std::string, std::string>> headers_;
    std::string body_;
    std::string content_type_;
    uint64_t content_length_ = 0;
    bool has_content_length_ = false;
    bool accept_ranges_ = false;
    bool keep_alive_ = true;
    bool has_last_modified_ = false;
    std::time_t last_modified_ = 0;
};
