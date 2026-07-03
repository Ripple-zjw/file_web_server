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

int main(int argc, char* argv[])
{
    auto cfg = parse_args(
        std::span(argv, static_cast<size_t>(argc)));

    if (cfg.help) {
        print_help(argv[0]);
        return 0;
    }

    // Create listening socket
    int listen_fd = create_listener(cfg.port);
    if (listen_fd < 0)
        return 1;

    std::printf("Listening on port %u, serving: %s\n",
                cfg.port, cfg.root_dir.c_str());

    // Optional TLS
    TlsContext tls_ctx;
    if (cfg.cert_file && cfg.key_file) {
        if (!tls_ctx.load_certificate(*cfg.cert_file, *cfg.key_file)) {
            std::fprintf(stderr, "error: failed to load TLS certificate\n");
            ::close(listen_fd);
            return 1;
        }
        std::printf("TLS enabled (cert: %s, key: %s)\n",
                    cfg.cert_file->c_str(), cfg.key_file->c_str());
    }

    // Create and run event loop
    EventLoop loop(cfg.root_dir, cfg.keep_alive_timeout);
    if (tls_ctx)
        loop.set_tls_context(&tls_ctx);

    loop.add_listener(listen_fd);
    loop.run();

    // Cleanup
    ::close(listen_fd);
    std::printf("\nServer shut down.\n");
    return 0;
}
