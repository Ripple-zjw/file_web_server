#include "file_server.h"

#include "http/mime_types.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
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
            if (end == hex + 2 && val > 0 && val < 128) {
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

    struct stat st;
    if (::lstat(full_path.c_str(), &st) != 0)
        return FileError::NOT_FOUND;

    // 处理符号链接：解析真实路径并验证安全性
    if (S_ISLNK(st.st_mode)) {
        char* real = ::realpath(full_path.c_str(), nullptr);
        if (!real) return FileError::FORBIDDEN;
        std::string resolved(real);
        std::free(real);
        if (!is_within_root(resolved, root_dir_))
            return FileError::FORBIDDEN;
        if (::stat(full_path.c_str(), &st) != 0)
            return FileError::NOT_FOUND;
    }

    // 目录请求：自动查找 index.html
    if (S_ISDIR(st.st_mode)) {
        auto index_path = full_path + "/index.html";
        struct stat ist;
        if (::stat(index_path.c_str(), &ist) == 0 && S_ISREG(ist.st_mode)) {
            st = ist;
            full_path = index_path;
        } else {
            return FileError::FORBIDDEN;
        }
    }

    // 仅服务普通文件，拒绝设备文件、fifo、socket 等
    if (!S_ISREG(st.st_mode))
        return FileError::FORBIDDEN;

    int fd = ::open(full_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return FileError::NOT_FOUND;

    auto mime = detect_mime_type(full_path);

    return FileInfo{
        fd,
        st.st_size,
        st.st_mtime,
        std::string{mime},
        std::move(full_path)
    };
}
