#include "http_response.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>

static const char* weekday_name(int wday) noexcept
{
    static constexpr const char* names[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    return names[wday % 7];
}

static const char* month_name(int mon) noexcept
{
    static constexpr const char* names[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    return names[mon % 12];
}

static std::string format_time(std::time_t t) noexcept
{
    struct tm gmt;
    gmtime_r(&t, &gmt);
    char buf[64];
    std::snprintf(buf, sizeof(buf),
        "%s, %02d %s %04d %02d:%02d:%02d GMT",
        weekday_name(gmt.tm_wday), gmt.tm_mday,
        month_name(gmt.tm_mon), gmt.tm_year + 1900,
        gmt.tm_hour, gmt.tm_min, gmt.tm_sec);
    return buf;
}

const char* HttpResponse::reason(Status s) noexcept
{
    switch (s) {
    case Status::OK:                    return "OK";
    case Status::PARTIAL_CONTENT:       return "Partial Content";
    case Status::NOT_MODIFIED:          return "Not Modified";
    case Status::BAD_REQUEST:           return "Bad Request";
    case Status::FORBIDDEN:             return "Forbidden";
    case Status::NOT_FOUND:             return "Not Found";
    case Status::METHOD_NOT_ALLOWED:    return "Method Not Allowed";
    case Status::RANGE_NOT_SATISFIABLE: return "Range Not Satisfiable";
    case Status::INTERNAL_ERROR:        return "Internal Server Error";
    case Status::NOT_IMPLEMENTED:       return "Not Implemented";
    }
    return "Unknown";
}

void HttpResponse::set_header(std::string name, std::string value) noexcept
{
    headers_.emplace_back(std::move(name), std::move(value));
}

void HttpResponse::set_content_type(std::string_view ct) noexcept
{
    content_type_ = std::string{ct};
}

void HttpResponse::set_content_length(uint64_t len) noexcept
{
    content_length_ = len;
    has_content_length_ = true;
}

void HttpResponse::set_last_modified(std::time_t tm) noexcept
{
    last_modified_ = tm;
    has_last_modified_ = true;
}

std::string HttpResponse::build_headers() const noexcept
{
    std::string h;
    h.reserve(512);

    // Status line
    h += "HTTP/1.1 ";
    h += std::to_string(static_cast<uint16_t>(status_));
    h += ' ';
    h += reason(status_);
    h += "\r\n";

    // Date
    h += "Date: ";
    h += format_time(std::time(nullptr));
    h += "\r\n";

    // Content-Type (not for 304)
    if (status_ != Status::NOT_MODIFIED && !content_type_.empty()) {
        h += "Content-Type: ";
        h += content_type_;
        h += "\r\n";
    }

    // Content-Length (not for 304)
    if (status_ != Status::NOT_MODIFIED && has_content_length_) {
        h += "Content-Length: ";
        h += std::to_string(content_length_);
        h += "\r\n";
    }

    // Last-Modified
    if (has_last_modified_) {
        h += "Last-Modified: ";
        h += format_time(last_modified_);
        h += "\r\n";
    }

    // Accept-Ranges
    if (accept_ranges_) {
        h += "Accept-Ranges: bytes\r\n";
    }

    // Connection
    h += "Connection: ";
    h += keep_alive_ ? "keep-alive\r\n" : "close\r\n";

    // Custom headers
    for (const auto& [k, v] : headers_) {
        h += k;
        h += ": ";
        h += v;
        h += "\r\n";
    }

    // End of headers
    h += "\r\n";

    return h;
}

void HttpResponse::reset() noexcept
{
    status_ = Status::OK;
    headers_.clear();
    body_.clear();
    content_type_.clear();
    content_length_ = 0;
    has_content_length_ = false;
    accept_ranges_ = false;
    keep_alive_ = true;
    has_last_modified_ = false;
    last_modified_ = 0;
}

std::string HttpResponse::error_body(Status s) noexcept
{
    const char* msg = reason(s);
    return "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
           "<title>" + std::to_string(static_cast<uint16_t>(s)) + " " + msg +
           "</title></head><body><h1>" +
           std::to_string(static_cast<uint16_t>(s)) + " " + msg +
           "</h1></body></html>";
}
