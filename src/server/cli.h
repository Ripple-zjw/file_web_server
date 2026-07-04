#pragma once

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>

/// 服务器运行配置，由命令行参数填充
struct ServerConfig {
    std::string root_dir;                    ///< 文件服务根目录
    uint16_t port = 8080;                    ///< 监听端口
    std::optional<std::string> cert_file;    ///< TLS 证书文件路径
    std::optional<std::string> key_file;     ///< TLS 私钥文件路径
    int keep_alive_timeout = 75;             ///< Keep-Alive 超时秒数
    bool help = false;                       ///< 是否显示帮助
};

/**
 * @brief 解析命令行参数，生成 ServerConfig。
 *
 * 支持的选项：--root、--port、--cert、--key、--timeout、--help/-h。
 * --cert 和 --key 必须成对出现，否则报错退出。遇到未知选项也直接退出。
 *
 * @param args 命令行参数 span（包括 argv[0] 程序名）
 * @return 解析完成的 ServerConfig 结构体，root_dir 默认为 "."
 */
[[nodiscard]] inline ServerConfig
parse_args(std::span<char*> args) noexcept
{
    ServerConfig cfg;

    // 获取下一个参数
    auto next_arg = [&](size_t& i) -> std::optional<std::string_view> {
        if (i + 1 >= args.size())
            return std::nullopt;
        return std::string_view{args[i + 1]};
    };

    for (size_t i = 1; i < args.size(); ++i) {
        std::string_view arg{args[i]};

        if (arg == "--help" || arg == "-h") {
            cfg.help = true;
            continue;
        }

        auto val = next_arg(i);
        if (!val) {
            std::cerr << "error: " << arg << " requires a value\n";
            std::exit(1);
        }

        if (arg == "--root") {
            cfg.root_dir = std::string{*val};
            ++i;
        } else if (arg == "--port") {
            cfg.port = static_cast<uint16_t>(std::atoi(
                std::string{*val}.c_str()));
            ++i;
        } else if (arg == "--cert") {
            cfg.cert_file = std::string{*val};
            ++i;
        } else if (arg == "--key") {
            cfg.key_file = std::string{*val};
            ++i;
        } else if (arg == "--timeout") {
            cfg.keep_alive_timeout = std::atoi(
                std::string{*val}.c_str());
            ++i;
        } else {
            std::cerr << "error: unknown option: " << arg << '\n';
            std::exit(1);
        }
    }

    if (cfg.cert_file.has_value() != cfg.key_file.has_value()) {
        std::cerr << "error: --cert and --key must be used together\n";
        std::exit(1);
    }

    if (cfg.root_dir.empty()) {
        cfg.root_dir = ".";
    }

    return cfg;
}

/**
 * @brief 打印命令行帮助信息。
 * @param prog 程序名（argv[0]）
 */
inline void print_help(const char* prog) noexcept
{
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --root <dir>     Root directory to serve (default: .)\n"
              << "  --port <num>     Port to listen on (default: 8080)\n"
              << "  --cert <file>    TLS certificate file (PEM)\n"
              << "  --key <file>     TLS private key file (PEM)\n"
              << "  --timeout <sec>  Keep-Alive timeout (default: 75)\n"
              << "  --help, -h       Show this help\n";
}
