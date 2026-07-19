#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <variant>

/**
 * @brief 已打开文件的信息，包含文件描述符和元数据。
 *
 * 独占文件描述符所有权（析构时自动 close），支持移动但不支持拷贝。
 * 可通过 release_fd() 主动转移 fd 所有权传出。
 */
struct FileInfo {
    int         fd = -1;              ///< 只读打开的文件描述符
    off_t       size = 0;             ///< 文件总大小（字节）
    std::time_t last_modified = 0;    ///< 最终修改时间（Unix 时间戳）
    std::string mime_type;            ///< 检测到的 MIME 类型
    std::string resolved_path;        ///< 经过解析的绝对路径

    FileInfo() = default;

    /// 构造并填充全部字段
    FileInfo(int f, off_t s, std::time_t tm, std::string mt, std::string rp) noexcept
        : fd(f), size(s), last_modified(tm),
          mime_type(std::move(mt)), resolved_path(std::move(rp)) {}

    FileInfo(const FileInfo&) = delete;
    FileInfo& operator=(const FileInfo&) = delete;

    /// 移动构造：转移 fd 所有权，源对象的 fd 置为 -1
    FileInfo(FileInfo&& other) noexcept
        : fd(other.fd), size(other.size), last_modified(other.last_modified),
          mime_type(std::move(other.mime_type)), resolved_path(std::move(other.resolved_path))
    {
        other.fd = -1;
    }

    /// 移动赋值：先关闭自身 fd，再转移源对象的所有权
    FileInfo& operator=(FileInfo&& other) noexcept {
        if (this != &other) {
            if (fd >= 0) ::close(fd);
            fd = other.fd; other.fd = -1;
            size = other.size;
            last_modified = other.last_modified;
            mime_type = std::move(other.mime_type);
            resolved_path = std::move(other.resolved_path);
        }
        return *this;
    }

    /// 析构时自动关闭文件描述符
    ~FileInfo() noexcept {
        if (fd >= 0) ::close(fd);
    }

    /**
     * @brief 释放文件描述符的所有权（调用者负责后续关闭）。
     * @return 文件描述符值，此后本对象的 fd 为 -1
     */
    int release_fd() noexcept {
        int f = fd; fd = -1; return f;
    }
};

/// 文件访问错误类型
enum class FileError : uint8_t {
    NOT_FOUND,    ///< 文件不存在
    FORBIDDEN,    ///< 权限不足或路径逃逸
    BAD_REQUEST,  ///< 路径包含非法字符
    IS_DIRECTORY, ///< 路径是目录（无 index.html），需生成目录列表
};

/// 目录条目信息
struct DirEntry {
    std::string name;          ///< 文件名
    bool        is_directory;  ///< 是否为目录
    off_t       size;          ///< 文件大小（字节）
    std::time_t last_modified; ///< 最后修改时间
};

/// 文件访问结果：成功返回 FileInfo，失败返回 FileError
using FileResult = std::variant<FileInfo, FileError>;

/**
 * @brief 文件服务器 —— 将 URL 路径映射到本地文件，执行安全检查后打开文件。
 *
 * 功能：
 * - URL 路径规范化（百分号解码、解析 ./ 和 ../、拒绝控制字符和 NULL 字节）
 * - 阻止访问以 '.' 开头的隐藏文件
 * - 符号链接解析后验证目标仍在根目录内
 * - 目录请求自动查找 index.html
 * - 仅服务普通文件
 * - 自动检测 MIME 类型
 */
class FileServer {
public:
    /**
     * @brief 构造文件服务器，将根目录规范化为绝对路径。
     * @param root_dir 文件服务的根目录
     */
    explicit FileServer(std::string root_dir) noexcept;

    /**
     * @brief 根据请求路径打开对应的文件。
     * @param request_path URL 路径（如 "/docs/page.html" 或 "/"）
     * @return 成功时返回 FileInfo（包含已打开的 fd 和元数据），失败返回 FileError
     */
    FileResult open(std::string_view request_path) noexcept;

    /// @return 文件服务根目录（绝对路径）
    const std::string& root_dir() const noexcept { return root_dir_; }

    /**
     * @brief 规范化 URL 路径：百分号解码、解析 '.' 和 '..'、拒绝非法字符。
     * @param path 原始 URL 路径
     * @return 规范化后的路径（如 "/docs/page.html"），非法输入返回空字符串
     */
    static std::string normalize(std::string_view path) noexcept;

    /**
     * @brief 获取目录下的文件列表（用于目录列表功能）。
     * @param request_path 规范化后的目录 URL 路径
     * @return 目录条目列表（已排序：目录在前，文件在后，各自按字母序）
     */
    std::vector<DirEntry> list_directory(std::string_view request_path) noexcept;

    /**
     * @brief 生成目录列表的 HTML 页面。
     * @param request_path   当前请求路径（用于标题显示）
     * @param entries        目录条目列表
     * @param show_parent    是否显示返回上级目录的链接
     * @return 完整的 HTML 页面字符串
     */
    static std::string build_directory_html(
        std::string_view request_path,
        const std::vector<DirEntry>& entries,
        bool show_parent) noexcept;

private:
    std::string root_dir_;

    /**
     * @brief 验证解析后的路径是否在根目录范围内。
     * @param resolved 解析后的绝对路径
     * @param root     根目录路径
     * @return true 表示路径安全
     */
    static bool is_within_root(std::string_view resolved,
                               std::string_view root) noexcept;
};
