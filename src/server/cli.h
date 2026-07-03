#pragma once

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>

struct ServerConfig {
    std::string root_dir;
    uint16_t port = 8080;
    std::optional<std::string> cert_file;
    std::optional<std::string> key_file;
    int keep_alive_timeout = 75;
    bool help = false;
};

[[nodiscard]] inline ServerConfig
parse_args(std::span<char*> args) noexcept
{
    ServerConfig cfg;

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
