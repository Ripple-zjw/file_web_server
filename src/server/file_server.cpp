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

FileServer::FileServer(std::string root_dir) noexcept
{
    // Canonicalize root directory
    char* real = realpath(root_dir.c_str(), nullptr);
    if (real) {
        root_dir_ = real;
        std::free(real);
    } else {
        root_dir_ = std::move(root_dir);
    }
}

std::string FileServer::normalize(std::string_view path) noexcept
{
    // URL percent-decode
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

    // Reject NUL bytes and control characters
    for (unsigned char c : decoded) {
        if (c == 0 || (c < 32 && c != '\t')) return {};
    }

    // Split by '/', resolve '.' and '..'
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
            else return {}; // escape above root
        } else {
            parts.push_back(std::move(seg));
        }
        start = end + 1;
    }

    // Hide files starting with '.'
    for (const auto& p : parts) {
        if (!p.empty() && p[0] == '.')
            return {};
    }

    std::string result;
    for (const auto& p : parts) {
        result += '/';
        result += p;
    }
    if (result.empty()) result = "/";
    return result;
}

bool FileServer::is_within_root(std::string_view resolved,
                                std::string_view root) noexcept
{
    auto rp = resolved.find(root);
    if (rp != 0) return false;
    // resolved must equal root or root + '/'
    if (resolved.size() == root.size()) return true;
    return resolved[root.size()] == '/';
}

FileResult FileServer::open(std::string_view request_path) noexcept
{
    auto normalized = normalize(request_path);
    if (normalized.empty())
        return FileError::BAD_REQUEST;

    auto full_path = root_dir_ + normalized;

    struct stat st;
    if (::lstat(full_path.c_str(), &st) != 0)
        return FileError::NOT_FOUND;

    // Handle symlinks
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

    // Directory: look for index.html
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

    // Only serve regular files
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
