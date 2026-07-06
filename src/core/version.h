#pragma once

/// 项目版本
#define WEB_SERVER_VERSION_MAJOR 1
#define WEB_SERVER_VERSION_MINOR 0
#define WEB_SERVER_VERSION_PATCH 0

/// 版本字符串
#define WEB_SERVER_VERSION "1.0.0"

/// 版本描述（用于 --version 输出）
#define WEB_SERVER_VERSION_STR "file_web_server " WEB_SERVER_VERSION \
    " (kqueue + HTTP/1.1 + optional TLS)"
