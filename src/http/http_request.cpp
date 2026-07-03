#include "http_request.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>

/**
 * @brief 从原始 HTTP 头部数据中解析请求行和所有头部。
 *
 * 解析流程：
 * 1. 定位 \r\n\r\n 分隔符确定头部块边界
 * 2. 解析请求行：方法(空格)路径(空格)版本
 * 3. 逐行解析头部：获取 "Name: Value" 格式的键值对，键转为小写存储
 *
 * @param raw 包含完整 HTTP 请求头的缓冲区
 * @return true 表示成功解析，false 表示格式不合法
 */
bool HttpRequest::parse(std::string_view raw) noexcept
{
    reset();

    auto crlfcrlf = raw.find("\r\n\r\n");
    if (crlfcrlf == std::string_view::npos)
        return false;

    auto header_block = raw.substr(0, crlfcrlf);

    // 解析请求行
    auto first_lf = header_block.find("\r\n");
    if (first_lf == std::string_view::npos)
        return false;

    auto request_line = header_block.substr(0, first_lf);

    // --- 解析方法 ---
    auto sp1 = request_line.find(' ');
    if (sp1 == std::string_view::npos) return false;
    raw_method_ = std::string{request_line.substr(0, sp1)};
    if (raw_method_ == "GET")
        method_ = Method::GET;
    else if (raw_method_ == "HEAD")
        method_ = Method::HEAD;
    else
        method_ = Method::UNKNOWN;

    // --- 解析路径 ---
    auto sp2 = request_line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) return false;
    path_ = std::string{request_line.substr(sp1 + 1, sp2 - sp1 - 1)};

    // --- 解析版本 ---
    version_ = std::string{request_line.substr(sp2 + 1)};

    // --- 解析头部 ---
    auto pos = first_lf + 2;
    while (pos < header_block.size()) {
        auto eol = header_block.find("\r\n", pos);
        if (eol == std::string_view::npos) break;
        auto line = header_block.substr(pos, eol - pos);
        pos = eol + 2;

        auto colon = line.find(": ");
        if (colon == std::string_view::npos)
            colon = line.find(':');
        if (colon == std::string_view::npos)
            continue;

        auto name  = line.substr(0, colon);
        auto value = line.substr(colon + 1);
        // 跳过冒号后的空白字符
        while (!value.empty() && (value[0] == ' ' || value[0] == '\t'))
            value.remove_prefix(1);

        // 头部名称转换为小写以支持大小写不敏感查找
        auto name_lc = std::string{name};
        for (auto& c : name_lc) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        headers_.emplace_back(std::move(name_lc), std::string{value});
    }

    return true;
}

/// @return 原始方法字符串（GET/HEAD），用于日志或调试
std::string_view HttpRequest::method_str() const noexcept
{
    return raw_method_;
}

/**
 * @brief 在头部列表中查找指定的头部名称（大小写不敏感）。
 * @param name 头部名称（应使用小写形式，如 "host"、"connection"）
 * @return 对应的值视图，未找到返回空视图
 */
std::string_view HttpRequest::header(std::string_view name) const noexcept
{
    for (const auto& [k, v] : headers_) {
        if (k == name) return v;
    }
    return {};
}

/**
 * @brief 根据 HTTP 版本和 Connection 头部判断是否保持连接。
 *
 * 规则：
 * - HTTP/1.1：默认 keep-alive，除非 Connection: close
 * - HTTP/1.0：默认 close，除非 Connection: keep-alive
 *
 * @return true 表示客户端期望保持连接
 */
bool HttpRequest::is_keep_alive() const noexcept
{
    auto conn = header("connection");
    if (version_ == "HTTP/1.1")
        return conn != "close";
    // HTTP/1.0
    return conn == "keep-alive";
}

/**
 * @brief 解析 Range 头部，提取请求的字节区间。
 *
 * 支持的格式：
 * - "bytes=start-end"  → 闭区间 [start, end]
 * - "bytes=start-"     → 从 start 到文件末尾
 * - "bytes=-suffix"    → 最后 suffix 字节
 *
 * 超过合理范围的请求（start >= file_size、start > end）返回 nullopt。
 *
 * @param file_size 被请求文件的总大小（字节）
 * @return 成功时返回 [start, end] 闭区间，失败返回 nullopt
 */
std::optional<std::pair<off_t, off_t>>
HttpRequest::parse_range(off_t file_size) const noexcept
{
    auto range_hdr = header("range");
    if (range_hdr.empty())
        return std::nullopt;

    constexpr auto prefix = "bytes=";
    if (range_hdr.size() < 7 ||
        range_hdr.substr(0, 6) != prefix)
        return std::nullopt;

    auto val = range_hdr.substr(6);
    auto dash = val.find('-');
    if (dash == std::string_view::npos)
        return std::nullopt;

    auto start_str = val.substr(0, dash);
    auto end_str   = val.substr(dash + 1);

    off_t start = 0, end = file_size - 1;

    if (start_str.empty()) {
        // 后缀范围：如 "-500" 表示最后 500 字节
        long suffix = 0;
        for (char c : end_str) {
            if (c < '0' || c > '9') return std::nullopt;
            suffix = suffix * 10 + (c - '0');
        }
        if (suffix <= 0) return std::nullopt;
        start = file_size - suffix;
        if (start < 0) start = 0;
    } else {
        for (char c : start_str) {
            if (c < '0' || c > '9') return std::nullopt;
            start = start * 10 + (c - '0');
        }
        if (!end_str.empty()) {
            end = 0;
            for (char c : end_str) {
                if (c < '0' || c > '9') return std::nullopt;
                end = end * 10 + (c - '0');
            }
        }
    }

    if (start >= file_size || start > end)
        return std::nullopt;

    if (end >= file_size) end = file_size - 1;

    return std::pair{start, end};
}

/**
 * @brief 解析 If-Modified-Since 头部为 UTC 时间戳。
 *
 * 使用 strptime 解析 RFC 2822 格式的时间字符串：
 * "Wed, 21 Oct 2015 07:28:00 GMT"
 *
 * @return 成功时返回 UTC 时间戳，失败返回 nullopt
 */
std::optional<std::time_t>
HttpRequest::parse_if_modified_since() const noexcept
{
    auto val = header("if-modified-since");
    if (val.empty()) return std::nullopt;

    struct tm tm = {};
    const char* result = strptime(
        std::string{val}.c_str(),
        "%a, %d %b %Y %H:%M:%S %Z",
        &tm);

    if (!result)
        return std::nullopt;

    // 转换为 time_t（假设输入为 GMT）
    auto t = timegm(&tm);
    return t;
}

/// 重置所有字段为初始状态
void HttpRequest::reset() noexcept
{
    method_ = Method::UNKNOWN;
    raw_method_.clear();
    path_.clear();
    version_.clear();
    headers_.clear();
}
