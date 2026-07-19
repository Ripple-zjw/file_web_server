#include "file_server.h"

#include "core/debug_log.h"
#include "http/mime_types.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <libgen.h>
#include <string>
#include <unistd.h>
#include <vector>

/**
 * @brief 构造文件服务器，将给定的根目录规范化为绝对路径。
 * @param root_dir 文件服务的根目录（相对或绝对路径均可）
 */
FileServer::FileServer(std::string root_dir) noexcept
{
    // 将根目录规范化为绝对路径
    char* real = realpath(root_dir.c_str(), nullptr);
    if (real) {
        root_dir_ = real;
        std::free(real);
    } else {
        root_dir_ = std::move(root_dir);
    }
    DEBUG_LOG("root=%s", root_dir_.c_str());
}

/**
 * @brief 规范化 URL 路径：百分号解码、解析 '.' 和 '..'、过滤非法字符。
 *
 * 处理步骤：
 * 1. 百分号解码（%XX → 实际字节），仅解码可见 ASCII 字符
 * 2. 拒绝 NULL 字节和控制字符（制表符除外）
 * 3. 按 '/' 分割路径段，解析 '.'（跳过）和 '..'（弹出上一级）
 * 4. 拒绝路径逃逸（'..' 超出根目录）
 * 5. 拒绝以 '.' 开头的隐藏文件名
 * 6. 重新组合为以 '/' 开头的规范路径
 *
 * @param path 原始 URL 路径
 * @return 规范化后的路径，非法输入返回空字符串
 */
std::string FileServer::normalize(std::string_view path) noexcept
{
    // ---- 步骤 1：URL 百分号解码 ----
    std::string decoded;
    decoded.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '%' && i + 2 < path.size()) {
            char hex[3] = {path[i + 1], path[i + 2], 0};
            char* end = nullptr;
            long val = std::strtol(hex, &end, 16);
            if (end == hex + 2 && val > 0) {
                decoded += static_cast<char>(val);
                i += 2;
                continue;
            }
        }
        decoded += path[i];
    }

    // ---- 步骤 2：拒绝非法字符 ----
    for (unsigned char c : decoded) {
        if (c == 0 || (c < 32 && c != '\t')) return {};
    }

    // ---- 步骤 3-4：按 '/' 分割并解析 '.' 和 '..' ----
    std::vector<std::string> parts;
    size_t start = 0;
    while (start < decoded.size()) {
        while (start < decoded.size() && decoded[start] == '/')
            ++start;
        if (start >= decoded.size()) break;
        auto end = decoded.find('/', start);
        if (end == std::string::npos) end = decoded.size();
        auto seg = decoded.substr(start, end - start);
        if (seg.empty()) continue;
        if (seg == ".") { start = end + 1; continue; }
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
            else return {}; // 路径逃逸：试图越过根目录
        } else {
            parts.push_back(std::move(seg));
        }
        start = end + 1;
    }

    // ---- 步骤 5：拒绝隐藏文件 ----
    for (const auto& p : parts) {
        if (!p.empty() && p[0] == '.')
            return {};
    }

    // ---- 步骤 6：重组为规范路径 ----
    std::string result;
    for (const auto& p : parts) {
        result += '/';
        result += p;
    }
    if (result.empty()) result = "/";
    return result;
}

/**
 * @brief 验证解析后的绝对路径是否在根目录范围内。
 *
 * 检查 resolved 是否以 root 开头，并且紧接的字符是 '\0'（完全匹配）或 '/'（子路径）。
 *
 * @param resolved 解析后的绝对路径
 * @param root     根目录路径（绝对路径）
 * @return true 表示路径安全，false 表示路径逃逸
 */
bool FileServer::is_within_root(std::string_view resolved,
                                std::string_view root) noexcept
{
    auto rp = resolved.find(root);
    if (rp != 0) return false;
    // resolved 必须等于 root，或以 root + '/' 开头
    if (resolved.size() == root.size()) return true;
    return resolved[root.size()] == '/';
}

/**
 * @brief 安全地打开请求路径对应的文件。
 *
 * 处理流程：
 * 1. 规范化请求路径
 * 2. 拼接为完整路径，lstat 检查文件存在
 * 3. 解析符号链接并验证目标在根目录内
 * 4. 目录请求自动查找 index.html
 * 5. 仅允许普通文件
 * 6. 以只读方式打开并检测 MIME 类型
 *
 * @param request_path URL 请求路径（如 "/docs/page.html"）
 * @return 成功返回 FileInfo，失败返回 FileError
 */
FileResult FileServer::open(std::string_view request_path) noexcept
{
    auto normalized = normalize(request_path);
    if (normalized.empty())
        return FileError::BAD_REQUEST;

    auto full_path = root_dir_ + normalized;
    DEBUG_LOG("request_path=%.*s normalized=%s full_path=%s",
              static_cast<int>(request_path.size()), request_path.data(),
              normalized.c_str(), full_path.c_str());

    struct stat st;
    if (::lstat(full_path.c_str(), &st) != 0)
        return FileError::NOT_FOUND;

    // 处理符号链接：解析真实路径并验证安全性
    if (S_ISLNK(st.st_mode)) {
        char* real = ::realpath(full_path.c_str(), nullptr);
        if (!real) return FileError::FORBIDDEN;
        std::string resolved(real);
        std::free(real);
        DEBUG_LOG("symlink resolved=%s", resolved.c_str());
        if (!is_within_root(resolved, root_dir_))
            return FileError::FORBIDDEN;
        if (::stat(full_path.c_str(), &st) != 0)
            return FileError::NOT_FOUND;
    }

    // 目录请求：没有 index.html 时返回目录列表标记
    if (S_ISDIR(st.st_mode)) {
        auto index_path = full_path + "/index.html";
        struct stat ist;
        if (::stat(index_path.c_str(), &ist) == 0 && S_ISREG(ist.st_mode)) {
            st = ist;
            full_path = index_path;
        } else {
            return FileError::IS_DIRECTORY;
        }
    }

    // 仅服务普通文件，拒绝设备文件、fifo、socket 等
    if (!S_ISREG(st.st_mode))
        return FileError::FORBIDDEN;

    int fd = ::open(full_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return FileError::NOT_FOUND;

    auto mime = detect_mime_type(full_path);
    DEBUG_LOG("fd=%d size=%lld mime=%.*s", fd,
              (long long)st.st_size,
              static_cast<int>(mime.size()), mime.data());

    return FileInfo{
        fd,
        st.st_size,
        st.st_mtime,
        std::string{mime},
        std::move(full_path)
    };
}

// ============================================================================
// 目录列表功能
// ============================================================================

/// 将文件大小格式化为人类可读的字符串
static std::string format_size(off_t size) noexcept
{
    char buf[32];
    if (size < 1024) {
        std::snprintf(buf, sizeof(buf), "%lld B", static_cast<long long>(size));
    } else if (size < 1024LL * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", size / 1024.0);
    } else if (size < 1024LL * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", size / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f GB", size / (1024.0 * 1024.0 * 1024.0));
    }
    return buf;
}

/// 将时间戳格式化为本地时间字符串
static std::string format_time(std::time_t t) noexcept
{
    struct tm result;
    ::localtime_r(&t, &result);
    char buf[32];
    ::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &result);
    return buf;
}

/// HTML 特殊字符转义
static std::string html_escape(std::string_view s) noexcept
{
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
        case '&':  out += "&amp;";  break;
        case '<':  out += "&lt;";   break;
        case '>':  out += "&gt;";   break;
        case '"':  out += "&quot;"; break;
        case '\'': out += "&#39;";  break;
        default:   out += static_cast<char>(c); break;
        }
    }
    return out;
}

/// URL 编码文件名（保留 `/` 不编码）
static std::string url_encode_path(std::string_view s) noexcept
{
    std::string out;
    out.reserve(s.size() + 8);
    constexpr const char hex[] = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out += static_cast<char>(c);
        } else if (c == ' ') {
            out += "%20";
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xf];
        }
    }
    return out;
}

/**
 * @brief 获取目录下的文件列表。
 *
 * 读取目录内容，过滤掉隐藏文件，获取每个条目的类型、大小和修改时间。
 * 结果排序规则：目录在前，文件在后；各自按字母序升序排列。
 *
 * @param request_path 请求的目录 URL 路径
 * @return 目录条目列表，失败时返回空 vector
 */
std::vector<DirEntry> FileServer::list_directory(std::string_view request_path) noexcept
{
    auto normalized = normalize(request_path);
    if (normalized.empty()) return {};

    auto dir_path = root_dir_ + normalized;
    if (dir_path.empty() || dir_path.back() != '/')
        dir_path += '/';

    DIR* dir = ::opendir(dir_path.c_str());
    if (!dir) return {};

    std::vector<DirEntry> entries;
    struct dirent* entry;
    while ((entry = ::readdir(dir)) != nullptr) {
        std::string name(entry->d_name);
        // 跳过 .、.. 和隐藏文件
        if (name.empty() || name[0] == '.') continue;

        auto full = dir_path + name;
        struct stat st;
        if (::stat(full.c_str(), &st) != 0) continue;

        DirEntry de;
        de.name = std::move(name);
        de.is_directory = S_ISDIR(st.st_mode);
        de.size = st.st_size;
        de.last_modified = st.st_mtime;
        entries.push_back(std::move(de));
    }
    ::closedir(dir);

    // 排序：目录在前，文件在后；各自按字母序
    std::sort(entries.begin(), entries.end(),
        [](const DirEntry& a, const DirEntry& b) noexcept {
            if (a.is_directory != b.is_directory)
                return a.is_directory > b.is_directory; // 目录排在前面
            return a.name < b.name;
        });

    DEBUG_LOG("dir=%.*s entries=%zu",
              static_cast<int>(request_path.size()), request_path.data(),
              entries.size());
    return entries;
}

/**
 * @brief 生成目录列表的 HTML 页面。
 *
 * 采用 GitHub 风格深色主题，响应式布局。
 * 包含图标化文件名、文件大小和修改时间三列。
 *
 * @param request_path 当前请求路径（用于标题和链接前缀）
 * @param entries      目录条目列表
 * @param show_parent  是否显示返回上级目录的链接（根目录不显示）
 * @return 完整的 HTML 页面字符串
 */
std::string FileServer::build_directory_html(
    std::string_view request_path,
    const std::vector<DirEntry>& entries,
    bool show_parent) noexcept
{
    // CSS 样式：深色主题，GitHub 风格文件浏览器
    constexpr std::string_view CSS = R"(
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Helvetica,Arial,sans-serif;background:#0d1117;color:#e6edf3;line-height:1.6;min-height:100vh}
.container{max-width:960px;margin:0 auto;padding:32px 24px}
h1{font-size:24px;font-weight:600;margin-bottom:24px;color:#e6edf3;display:flex;align-items:center;gap:10px}
h1 .icon{font-size:26px}
table{width:100%;border-collapse:collapse;background:#161b22;border:1px solid #30363d;border-radius:8px;overflow:hidden}
thead th{text-align:left;padding:12px 16px;font-size:13px;font-weight:600;color:#8b949e;border-bottom:1px solid #30363d;background:#0d1117;user-select:none}
tbody tr{border-bottom:1px solid #21262d;transition:background .12s ease}
tbody tr:last-child{border-bottom:none}
tbody tr:hover{background:#1c2128}
td{padding:10px 16px;font-size:14px}
td.name a{color:#58a6ff;text-decoration:none;display:inline-flex;align-items:center;gap:8px}
td.name a:hover{color:#79c0ff;text-decoration:underline}
td.size{color:#8b949e;font-variant-numeric:tabular-nums;width:100px}
td.date{color:#8b949e;font-variant-numeric:tabular-nums;width:170px}
tr.parent td{border-bottom:1px solid #30363d;padding-top:10px;padding-bottom:10px}
tr.parent td.name a{color:#e6edf3;font-weight:500}
.icon-dir{color:#58a6ff}
.icon-file{color:#8b949e}
footer{margin-top:24px;text-align:center;font-size:12px;color:#484f58}
@media(max-width:600px){.container{padding:16px 12px}h1{font-size:20px}td.date{display:none}td.size{width:70px}}
)";

    std::string html;
    // 预分配：目录数 * 120 字节 + 固定部分 2KB
    html.reserve(2048 + entries.size() * 120);

    html += "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n";
    html += "<meta charset=\"utf-8\">\n";
    html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    html += "<title>Index of ";
    html += html_escape(request_path);
    html += "</title>\n<style>\n";
    html += CSS;
    html += "</style>\n</head>\n<body>\n<div class=\"container\">\n";
    html += "<h1><span class=\"icon\">&#x1F4C1;</span> Index of ";
    html += html_escape(request_path);
    html += "</h1>\n<table>\n<thead>\n<tr>";
    html += "<th>Name</th><th class=\"size\">Size</th><th class=\"date\">Modified</th>";
    html += "</tr>\n</thead>\n<tbody>\n";

    // 上级目录链接（非根目录时显示）
    if (show_parent) {
        html += "<tr class=\"parent\">\n";
        html += "<td class=\"name\"><a href=\"../\">&#x1F4C2; ../</a></td>\n";
        html += "<td class=\"size\">-</td>\n";
        html += "<td class=\"date\">-</td>\n</tr>\n";
    }

    // 生成文件条目
    for (const auto& e : entries) {
        html += "<tr>\n<td class=\"name\"><a href=\"";
        // 构建安全的 URL 路径
        if (show_parent) {
            html += url_encode_path(e.name);
        } else {
            // 根目录：路径已含开头的 /
            auto href = std::string{request_path};
            if (href.empty() || href.back() != '/') href += '/';
            href += url_encode_path(e.name);
            html += href;
        }
        if (e.is_directory) html += '/';
        html += "\">";
        // 图标
        html += e.is_directory
            ? "<span class=\"icon-dir\">&#x1F4C2;</span> "
            : "<span class=\"icon-file\">&#x1F4C4;</span> ";
        // 转义后的文件名
        html += html_escape(e.name);
        if (e.is_directory) html += '/';
        html += "</a></td>\n";
        html += "<td class=\"size\">";
        html += e.is_directory ? "-" : format_size(e.size);
        html += "</td>\n";
        html += "<td class=\"date\">";
        html += format_time(e.last_modified);
        html += "</td>\n</tr>\n";
    }

    html += "</tbody>\n</table>\n";
    html += "<footer>web_server_cpp/1.0</footer>\n";
    html += "</div>\n</body>\n</html>";

    return html;
}
