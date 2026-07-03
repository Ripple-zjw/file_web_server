#include "http_request.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>

bool HttpRequest::parse(std::string_view raw) noexcept
{
    reset();

    auto crlfcrlf = raw.find("\r\n\r\n");
    if (crlfcrlf == std::string_view::npos)
        return false;

    auto header_block = raw.substr(0, crlfcrlf);

    // Parse request line
    auto first_lf = header_block.find("\r\n");
    if (first_lf == std::string_view::npos)
        return false;

    auto request_line = header_block.substr(0, first_lf);

    // Method
    auto sp1 = request_line.find(' ');
    if (sp1 == std::string_view::npos) return false;
    raw_method_ = std::string{request_line.substr(0, sp1)};
    if (raw_method_ == "GET")
        method_ = Method::GET;
    else if (raw_method_ == "HEAD")
        method_ = Method::HEAD;
    else
        method_ = Method::UNKNOWN;

    // Path
    auto sp2 = request_line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) return false;
    path_ = std::string{request_line.substr(sp1 + 1, sp2 - sp1 - 1)};

    // Version
    version_ = std::string{request_line.substr(sp2 + 1)};

    // Headers
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
        // Skip leading whitespace after colon
        while (!value.empty() && (value[0] == ' ' || value[0] == '\t'))
            value.remove_prefix(1);

        auto name_lc = std::string{name};
        for (auto& c : name_lc) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        headers_.emplace_back(std::move(name_lc), std::string{value});
    }

    return true;
}

std::string_view HttpRequest::method_str() const noexcept
{
    return raw_method_;
}

std::string_view HttpRequest::header(std::string_view name) const noexcept
{
    for (const auto& [k, v] : headers_) {
        if (k == name) return v;
    }
    return {};
}

bool HttpRequest::is_keep_alive() const noexcept
{
    auto conn = header("connection");
    if (version_ == "HTTP/1.1")
        return conn != "close";
    // HTTP/1.0
    return conn == "keep-alive";
}

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
        // Suffix range: -500
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

    // Convert to time_t (assume GMT)
    auto t = timegm(&tm);
    return t;
}

void HttpRequest::reset() noexcept
{
    method_ = Method::UNKNOWN;
    raw_method_.clear();
    path_.clear();
    version_.clear();
    headers_.clear();
}
