#pragma once

/**
 * @brief 调试日志宏 —— Debug 构建时输出文件、行号、函数名和消息，Release 构建时完全消除。
 *
 * 使用方式：DEBUG_LOG("accept fd=%d", fd);
 * 输出格式：[file:line func()] message
 *
 * CMAKE_BUILD_TYPE=Debug 或未指定 Release 时生效。
 * Release 构建时编译器会将整个宏体优化掉（空的可变参数宏）。
 *
 * 使用 __VA_OPT__（C++20 标准特性）实现可选参数，无需 GNU 扩展。
 */

#ifndef NDEBUG

#include <cstdio>
#include <utility>

// 内部实现：将文件名从全路径中截取最后一段
namespace detail {
inline constexpr const char* filename(const char* path) noexcept {
    const char* slash = path;
    while (*path) { if (*path++ == '/') slash = path; }
    return slash;
}
} // namespace detail

// 带 printf 风格格式化参数的日志宏（C++20 __VA_OPT__）
#define DEBUG_LOG(fmt, ...)                                                     \
    do {                                                                        \
        std::fprintf(stderr, "[%s:%d %s()] " fmt "\n",                         \
                     detail::filename(__FILE__),                                \
                     __LINE__, __func__                                          \
                     __VA_OPT__(,) __VA_ARGS__);                                \
    } while (0)

#else
// Release 构建：宏展开为空，零开销
#define DEBUG_LOG(fmt, ...) do {} while (0)
#endif

/**
 * @brief 条件日志宏 —— 仅在条件满足时输出日志（Debug 构建下）。
 * 示例：DEBUG_LOG_IF(data > threshold, "data=%zu exceeded", data);
 */
#define DEBUG_LOG_IF(cond, fmt, ...) \
    do { if (cond) { DEBUG_LOG(fmt __VA_OPT__(,) __VA_ARGS__); } } while (0)
