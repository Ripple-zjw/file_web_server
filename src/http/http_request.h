#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class HttpRequest {
public:
    enum class Method : uint8_t { GET, HEAD, UNKNOWN };

    [[nodiscard]] bool parse(std::string_view raw) noexcept;

    std::string_view method_str() const noexcept;
    Method           method()    const noexcept { return method_; }
    std::string_view path()      const noexcept { return path_; }
    std::string_view version()   const noexcept { return version_; }

    std::string_view header(std::string_view name) const noexcept;

    [[nodiscard]] bool is_keep_alive() const noexcept;

    [[nodiscard]] std::optional<std::pair<off_t, off_t>>
    parse_range(off_t file_size) const noexcept;

    [[nodiscard]] std::optional<std::time_t>
    parse_if_modified_since() const noexcept;

    void reset() noexcept;

private:
    Method method_ = Method::UNKNOWN;
    std::string raw_method_;
    std::string path_;
    std::string version_;
    std::vector<std::pair<std::string, std::string>> headers_;
};
