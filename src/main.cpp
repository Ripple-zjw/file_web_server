#include "core/debug_log.h"
#include "core/event_loop.h"
#include "core/tls_context.h"
#include "server/cli.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

/**
 * @brief 创建并绑定监听 socket。
 *
 * 流程：创建 IPv4 TCP socket → 设置 SO_REUSEADDR → 绑定到指定端口 →
 * 调用 listen 进入监听状态。返回的 fd 为阻塞模式，调用方负责设为非阻塞。
 *
 * @param port 监听的端口号
 * @return 成功返回监听 socket 的 fd，失败返回 -1（错误信息已通过 perror 输出）
 */
[[nodiscard]] static int
create_listener(uint16_t port) noexcept
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return -1;
    }

    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr),
               sizeof(addr)) < 0)
    {
        std::perror("bind");
        ::close(fd);
        return -1;
    }

    if (::listen(fd, SOMAXCONN) < 0) {
        std::perror("listen");
        ::close(fd);
        return -1;
    }

    return fd;
}

/**
 * @brief 主函数：解析命令行 → 创建监听 socket → 可选 TLS → 启动事件循环。
 *
 * @param argc 参数数量
 * @param argv 参数数组
 * @return 0 正常退出，1 表示启动错误
 */
int main(int argc, char* argv[])
{
    auto cfg = parse_args(
        std::span(argv, static_cast<size_t>(argc)));

    if (cfg.help) {
        print_help(argv[0]);
        return 0;
    }

    DEBUG_LOG("port=%u root=%s", cfg.port, cfg.root_dir.c_str());

    // 创建监听 socket
    int listen_fd = create_listener(cfg.port);
    if (listen_fd < 0)
        return 1;

    std::printf("Listening on port %u, serving: %s\n",
                cfg.port, cfg.root_dir.c_str());

    // 可选 TLS —— 若同时提供证书和密钥则启用
    TlsContext tls_ctx;
    if (cfg.cert_file && cfg.key_file) {
        DEBUG_LOG("loading cert=%s key=%s",
                  cfg.cert_file->c_str(), cfg.key_file->c_str());
        if (!tls_ctx.load_certificate(*cfg.cert_file, *cfg.key_file)) {
            std::fprintf(stderr, "error: failed to load TLS certificate\n");
            ::close(listen_fd);
            return 1;
        }
        std::printf("TLS enabled (cert: %s, key: %s)\n",
                    cfg.cert_file->c_str(), cfg.key_file->c_str());
    }

    // 创建并运行事件循环
    DEBUG_LOG("starting event loop");
    EventLoop loop(cfg.root_dir, cfg.keep_alive_timeout);
    if (tls_ctx)
        loop.set_tls_context(&tls_ctx);

    loop.add_listener(listen_fd);
    loop.run();

    // 清理：关闭监听 socket（事件循环退出后 fd 仍需手动关闭）
    ::close(listen_fd);
    std::printf("\nServer shut down.\n");
    DEBUG_LOG("server exited");
    return 0;
}
