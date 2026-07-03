#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class HttpResponse {
public:
    enum class Status : uint16_t {
        OK = 200,
        PARTIAL_CONTENT = 206,
        NOT_MODIFIED = 304,
        BAD_REQUEST = 400,
        FORBIDDEN = 403,
        NOT_FOUND = 404,
        METHOD_NOT_ALLOWED = 405,
        RANGE_NOT_SATISFIABLE = 416,
        INTERNAL_ERROR = 500,
        NOT_IMPLEMENTED = 501,
    };

    void set_status(Status s) noexcept { status_ = s; }
    void set_header(std::string name, std::string value) noexcept;
    void set_content_type(std::string_view ct) noexcept;
    void set_content_length(uint64_t len) noexcept;
    void set_last_modified(std::time_t tm) noexcept;
    void set_accept_ranges(bool v) noexcept { accept_ranges_ = v; }
    void set_keep_alive(bool v) noexcept { keep_alive_ = v; }
    void set_body(std::string b) noexcept { body_ = std::move(b); }

    Status status() const noexcept { return status_; }
    bool   has_body() const noexcept { return !body_.empty(); }
    std::string_view body() const noexcept { return body_; }

    std::string build_headers() const noexcept;
    void reset() noexcept;

    static const char* reason(Status s) noexcept;
    static std::string error_body(Status s) noexcept;

private:
    Status status_ = Status::OK;
    std::vector<std::pair<std::string, std::string>> headers_;
    std::string body_;
    std::string content_type_;
    uint64_t content_length_ = 0;
    bool has_content_length_ = false;
    bool accept_ranges_ = false;
    bool keep_alive_ = true;
    bool has_last_modified_ = false;
    std::time_t last_modified_ = 0;
};
