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

struct FileInfo {
    int         fd = -1;
    off_t       size = 0;
    std::time_t last_modified = 0;
    std::string mime_type;
    std::string resolved_path;

    FileInfo() = default;
    FileInfo(int f, off_t s, std::time_t tm, std::string mt, std::string rp) noexcept
        : fd(f), size(s), last_modified(tm),
          mime_type(std::move(mt)), resolved_path(std::move(rp)) {}

    FileInfo(const FileInfo&) = delete;
    FileInfo& operator=(const FileInfo&) = delete;

    FileInfo(FileInfo&& other) noexcept
        : fd(other.fd), size(other.size), last_modified(other.last_modified),
          mime_type(std::move(other.mime_type)), resolved_path(std::move(other.resolved_path))
    {
        other.fd = -1;
    }

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

    ~FileInfo() noexcept {
        if (fd >= 0) ::close(fd);
    }

    int release_fd() noexcept {
        int f = fd; fd = -1; return f;
    }
};

enum class FileError : uint8_t {
    NOT_FOUND,
    FORBIDDEN,
    BAD_REQUEST,
};

using FileResult = std::variant<FileInfo, FileError>;

class FileServer {
public:
    explicit FileServer(std::string root_dir) noexcept;

    FileResult open(std::string_view request_path) noexcept;

    const std::string& root_dir() const noexcept { return root_dir_; }

private:
    std::string root_dir_;

    static std::string normalize(std::string_view path) noexcept;

    static bool is_within_root(std::string_view resolved,
                               std::string_view root) noexcept;
};
