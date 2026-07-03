#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief HTTP 请求解析器，从原始字节流中提取方法、路径、版本和头部。
 *
 * 仅支持 GET 和 HEAD 方法，支持解析 Range 和 If-Modified-Since 头部。
 * 所有解析在原始缓冲区上以零拷贝方式完成（内部存储副本）。
 */
class HttpRequest {
public:
    /// HTTP 方法，仅支持 GET 和 HEAD
    enum class Method : uint8_t { GET, HEAD, UNKNOWN };

    /**
     * @brief 从原始 HTTP 数据中解析请求。
     * @param raw 包含完整 HTTP 请求头的缓冲区（需包含终止标记 \r\n\r\n）
     * @return true 表示成功解析了请求行和所有头部，false 表示格式错误
     */
    [[nodiscard]] bool parse(std::string_view raw) noexcept;

    /// @return 原始方法字符串（GET/HEAD），用于日志或调试
    std::string_view method_str() const noexcept;
    /// @return 解析后的方法枚举值
    Method           method()    const noexcept { return method_; }
    /// @return URL 路径部分（含查询参数，不含片段），未经解码
    std::string_view path()      const noexcept { return path_; }
    /// @return HTTP 版本字符串（如 "HTTP/1.1"）
    std::string_view version()   const noexcept { return version_; }

    /**
     * @brief 根据名称查找请求头部值（大小写不敏感查找）。
     * @param name 头部名称（应使用小写形式，如 "host"、"connection"）
     * @return 对应的值视图，未找到时返回空视图
     */
    std::string_view header(std::string_view name) const noexcept;

    /**
     * @brief 判断客户端是否请求 keep-alive 连接。
     *
     * HTTP/1.1 默认 keep-alive（除非 Connection: close），
     * HTTP/1.0 仅在 Connection: keep-alive 时启用。
     *
     * @return true 表示应保持连接
     */
    [[nodiscard]] bool is_keep_alive() const noexcept;

    /**
     * @brief 解析 Range 头部为字节区间。
     * @param file_size 请求文件的总大小（字节数），用于计算默认值和验证区间
     * @return 成功时返回 [start, end] 闭区间，未指定或无 Range 头部时返回 nullopt
     */
    [[nodiscard]] std::optional<std::pair<off_t, off_t>>
    parse_range(off_t file_size) const noexcept;

    /**
     * @brief 解析 If-Modified-Since 头部为时间戳。
     * @return 时间戳（UTC），无此头部或格式错误时返回 nullopt
     */
    [[nodiscard]] std::optional<std::time_t>
    parse_if_modified_since() const noexcept;

    /// 重置解析器到初始状态，复用时调用
    void reset() noexcept;

private:
    Method method_ = Method::UNKNOWN;
    std::string raw_method_;
    std::string path_;
    std::string version_;
    /// 头部列表，键为小写形式
    std::vector<std::pair<std::string, std::string>> headers_;
};
