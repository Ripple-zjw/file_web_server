#pragma once

#include "connection.h"
#include "connection_pool.h"
#include "server/file_server.h"
#include "tls_context.h"

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/event.h>
#include <vector>

extern "C" {
inline std::sig_atomic_t g_running = 1;
inline void handle_signal(int) { g_running = 0; }
}

class EventLoop {
public:
    static constexpr int MAX_EVENTS = 1024;

    EventLoop(std::string root_dir, int keep_alive_timeout) noexcept;
    ~EventLoop() noexcept;

    void set_tls_context(TlsContext* ctx) noexcept { tls_ctx_ = ctx; }
    void add_listener(int fd) noexcept;
    void run() noexcept;

private:
    using Pool = ConnectionPool<Connection>;

    int kq_ = -1;
    int listen_fd_ = -1;
    TlsContext* tls_ctx_ = nullptr;
    FileServer file_server_;
    Pool pool_;
    int keep_alive_timeout_ = 75;

    // Tags for udata identification
    char listener_tag_ = 'L';
    char timer_tag_    = 'T';

    // Changelist for batching kqueue modifications
    std::vector<struct kevent> changelist_;

    // Event buffer
    struct kevent events_[MAX_EVENTS];

    void flush_changelist() noexcept;
    void add_event(uintptr_t ident, int16_t filter, uint16_t flags,
                   void* udata) noexcept;

    void dispatch(const struct kevent& ev) noexcept;
    void handle_accept() noexcept;
    void handle_timer() noexcept;
    void handle_connection_event(Connection* conn,
                                 const struct kevent& ev) noexcept;

    void process_connection(Connection* conn) noexcept;
    void prepare_response(Connection* conn) noexcept;
    void update_events(Connection* conn, Connection::Want want) noexcept;

    Connection* create_connection(int fd) noexcept;
    void destroy_connection(Connection* conn) noexcept;
};
