#pragma once

#include <algorithm>
#include <array>
#include <string_view>

struct MimeEntry {
    const char* ext;
    const char* mime;
};

inline constexpr std::array<MimeEntry, 55> MIME_TABLE = {{
    {".aac",     "audio/aac"},
    {".avif",    "image/avif"},
    {".avi",     "video/x-msvideo"},
    {".bmp",     "image/bmp"},
    {".bz2",     "application/x-bzip2"},
    {".csv",     "text/csv"},
    {".doc",     "application/msword"},
    {".docx",    "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {".eot",     "application/vnd.ms-fontobject"},
    {".epub",    "application/epub+zip"},
    {".flac",    "audio/flac"},
    {".flv",     "video/x-flv"},
    {".gif",     "image/gif"},
    {".gz",      "application/gzip"},
    {".htm",     "text/html; charset=utf-8"},
    {".html",    "text/html; charset=utf-8"},
    {".ico",     "image/x-icon"},
    {".jpeg",    "image/jpeg"},
    {".jpg",     "image/jpeg"},
    {".js",      "application/javascript"},
    {".json",    "application/json"},
    {".m4a",     "audio/mp4"},
    {".mkv",     "video/x-matroska"},
    {".mov",     "video/quicktime"},
    {".mp3",     "audio/mpeg"},
    {".mp4",     "video/mp4"},
    {".mpeg",    "video/mpeg"},
    {".mpg",     "video/mpeg"},
    {".ogg",     "audio/ogg"},
    {".otf",     "font/otf"},
    {".pdf",     "application/pdf"},
    {".png",     "image/png"},
    {".ppt",     "application/vnd.ms-powerpoint"},
    {".pptx",    "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {".rar",     "application/vnd.rar"},
    {".svg",     "image/svg+xml"},
    {".svgz",    "image/svg+xml"},
    {".swf",     "application/x-shockwave-flash"},
    {".tar",     "application/x-tar"},
    {".tif",     "image/tiff"},
    {".tiff",    "image/tiff"},
    {".toml",    "application/toml"},
    {".ttf",     "font/ttf"},
    {".txt",     "text/plain"},
    {".wav",     "audio/wave"},
    {".webm",    "video/webm"},
    {".webp",    "image/webp"},
    {".woff",    "font/woff"},
    {".woff2",   "font/woff2"},
    {".xls",     "application/vnd.ms-excel"},
    {".xlsx",    "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {".xml",     "application/xml"},
    {".yaml",    "application/yaml"},
    {".yml",     "application/yaml"},
    {".zip",     "application/zip"},
}};

inline constexpr std::string_view FALLBACK_MIME = "application/octet-stream";

[[nodiscard]] inline std::string_view
detect_mime_type(std::string_view path) noexcept
{
    auto dot = path.rfind('.');
    if (dot == std::string_view::npos)
        return FALLBACK_MIME;

    auto ext = path.substr(dot);
    auto it = std::lower_bound(
        MIME_TABLE.begin(), MIME_TABLE.end(), ext,
        [](const MimeEntry& e, std::string_view key) {
            return std::string_view{e.ext} < key;
        });

    if (it != MIME_TABLE.end() && it->ext == ext)
        return it->mime;

    return FALLBACK_MIME;
}

