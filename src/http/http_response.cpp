#include "http_response.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>

/**
 * @brief 将星期数（0=周日）转换为缩写名称。
 * @param wday tm_wday 值
 * @return "Sun"、"Mon" 等三字母缩写
 */
static const char* weekday_name(int wday) noexcept
{
    static constexpr const char* names[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    return names[wday % 7];
}

/**
 * @brief 将月份数（0=一月）转换为缩写名称。
 * @param mon tm_mon 值
 * @return "Jan"、"Feb" 等三字母缩写
 */
static const char* month_name(int mon) noexcept
{
    static constexpr const char* names[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    return names[mon % 12];
}

/**
 * @brief 将 Unix 时间戳格式化为 RFC 2822 日期字符串（GMT）。
 * @param t Unix 时间戳
 * @return 格式如 "Wed, 21 Oct 2015 07:28:00 GMT"
 */
static std::string format_time(std::time_t t) noexcept
{
    struct tm gmt;
    gmtime_r(&t, &gmt);
    char buf[64];
    std::snprintf(buf, sizeof(buf),
        "%s, %02d %s %04d %02d:%02d:%02d GMT",
        weekday_name(gmt.tm_wday), gmt.tm_mday,
        month_name(gmt.tm_mon), gmt.tm_year + 1900,
        gmt.tm_hour, gmt.tm_min, gmt.tm_sec);
    return buf;
}

/**
 * @brief 将 HTTP 状态码转换为 RFC 7231 原因短语。
 * @param s HTTP 状态码枚举
 * @return 对应的英文原因短语（如 "OK"、"Not Found"）或 "Unknown"
 */
const char* HttpResponse::reason(Status s) noexcept
{
    switch (s) {
    case Status::OK:                    return "OK";
    case Status::PARTIAL_CONTENT:       return "Partial Content";
    case Status::NOT_MODIFIED:          return "Not Modified";
    case Status::BAD_REQUEST:           return "Bad Request";
    case Status::FORBIDDEN:             return "Forbidden";
    case Status::NOT_FOUND:             return "Not Found";
    case Status::METHOD_NOT_ALLOWED:    return "Method Not Allowed";
    case Status::RANGE_NOT_SATISFIABLE: return "Range Not Satisfiable";
    case Status::INTERNAL_ERROR:        return "Internal Server Error";
    case Status::NOT_IMPLEMENTED:       return "Not Implemented";
    }
    return "Unknown";
}

/**
 * @brief 添加自定义 HTTP 头部。
 * @param name  头部名称
 * @param value 头部值
 */
void HttpResponse::set_header(std::string name, std::string value) noexcept
{
    headers_.emplace_back(std::move(name), std::move(value));
}

/// @param ct Content-Type 值（如 "text/html; charset=utf-8"）
void HttpResponse::set_content_type(std::string_view ct) noexcept
{
    content_type_ = std::string{ct};
}

/// @param len 响应体的字节长度
void HttpResponse::set_content_length(uint64_t len) noexcept
{
    content_length_ = len;
    has_content_length_ = true;
}

/// @param tm 文件的最后修改时间（Unix 时间戳）
void HttpResponse::set_last_modified(std::time_t tm) noexcept
{
    last_modified_ = tm;
    has_last_modified_ = true;
}

/**
 * @brief 构建完整的 HTTP 响应头字符串。
 *
 * 生成顺序：
 * 1. 状态行（HTTP/1.1 200 OK\r\n）
 * 2. Date 头部（当前时间）
 * 3. Content-Type（非 304 时输出）
 * 4. Content-Length（非 304 时输出）
 * 5. Last-Modified（如已设置）
 * 6. Accept-Ranges（如已设置）
 * 7. Connection 头部（keep-alive 或 close）
 * 8. 自定义头部
 * 9. 终止空行 \r\n
 *
 * @return 可直接通过 socket 发送的响应头字符串
 */
std::string HttpResponse::build_headers() const noexcept
{
    std::string h;
    h.reserve(512);

    // 状态行
    h += "HTTP/1.1 ";
    h += std::to_string(static_cast<uint16_t>(status_));
    h += ' ';
    h += reason(status_);
    h += "\r\n";

    // Date
    h += "Date: ";
    h += format_time(std::time(nullptr));
    h += "\r\n";

    // Content-Type（304 不输出）
    if (status_ != Status::NOT_MODIFIED && !content_type_.empty()) {
        h += "Content-Type: ";
        h += content_type_;
        h += "\r\n";
    }

    // Content-Length（304 不输出）
    if (status_ != Status::NOT_MODIFIED && has_content_length_) {
        h += "Content-Length: ";
        h += std::to_string(content_length_);
        h += "\r\n";
    }

    // Last-Modified
    if (has_last_modified_) {
        h += "Last-Modified: ";
        h += format_time(last_modified_);
        h += "\r\n";
    }

    // Accept-Ranges
    if (accept_ranges_) {
        h += "Accept-Ranges: bytes\r\n";
    }

    // Connection
    h += "Connection: ";
    h += keep_alive_ ? "keep-alive\r\n" : "close\r\n";

    // 自定义头部
    for (const auto& [k, v] : headers_) {
        h += k;
        h += ": ";
        h += v;
        h += "\r\n";
    }

    // 头部结束空行
    h += "\r\n";

    return h;
}

/// 重置所有字段为默认值
void HttpResponse::reset() noexcept
{
    status_ = Status::OK;
    headers_.clear();
    body_.clear();
    content_type_.clear();
    content_length_ = 0;
    has_content_length_ = false;
    accept_ranges_ = false;
    keep_alive_ = true;
    has_last_modified_ = false;
    last_modified_ = 0;
}

/**
 * @brief 生成包含状态码和原因短语的 HTML 错误页面。
 *
 * 页面格式：
 * <!DOCTYPE html><html><head><meta charset="utf-8"><title>404 Not Found</title>
 * </head><body><h1>404 Not Found</h1></body></html>
 *
 * @param s HTTP 状态码
 * @return 完整的 HTML 错误页面字符串
 */
std::string HttpResponse::error_body(Status s) noexcept
{
    const char* msg = reason(s);
    return "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
           "<title>" + std::to_string(static_cast<uint16_t>(s)) + " " + msg +
           "</title></head><body><h1>" +
           std::to_string(static_cast<uint16_t>(s)) + " " + msg +
           "</h1></body></html>";
}
