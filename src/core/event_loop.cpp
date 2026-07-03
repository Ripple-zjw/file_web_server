#include "event_loop.h"

#include "http/http_response.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

EventLoop::EventLoop(std::string root_dir, int keep_alive_timeout) noexcept
    : file_server_(std::move(root_dir))
    , keep_alive_timeout_(keep_alive_timeout)
{
    kq_ = kqueue();
}

EventLoop::~EventLoop() noexcept
{
    if (kq_ >= 0) ::close(kq_);
}

void EventLoop::add_event(uintptr_t ident, int16_t filter, uint16_t flags,
                          void* udata) noexcept
{
    struct kevent ev;
    EV_SET(&ev, ident, filter, flags, 0, 0, udata);
    changelist_.push_back(ev);
}

void EventLoop::flush_changelist() noexcept
{
    if (changelist_.empty()) return;
    kevent(kq_, changelist_.data(), static_cast<int>(changelist_.size()),
           nullptr, 0, nullptr);
    changelist_.clear();
}

void EventLoop::add_listener(int fd) noexcept
{
    listen_fd_ = fd;

    // Set non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    // Register listen socket for reads (incoming connections)
    add_event(static_cast<uintptr_t>(fd), EVFILT_READ, EV_ADD | EV_ENABLE,
              &listener_tag_);

    // Register 1-second periodic timer for keep-alive sweep
    add_event(0, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_CLEAR,
              &timer_tag_);

    flush_changelist();
}

void EventLoop::run() noexcept
{
    // Ignore SIGPIPE
    signal(SIGPIPE, SIG_IGN);

    // Set up SIGINT/SIGTERM handler without SA_RESTART
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    while (g_running) {
        int n = kevent(kq_, changelist_.data(),
                       static_cast<int>(changelist_.size()),
                       events_, MAX_EVENTS, nullptr);
        changelist_.clear();

        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; ++i)
            dispatch(events_[i]);
    }
}

void EventLoop::dispatch(const struct kevent& ev) noexcept
{
    if (ev.flags & EV_ERROR) {
        // Error on fd — close if it's a connection
        if (ev.udata != &listener_tag_ && ev.udata != &timer_tag_) {
            auto* conn = static_cast<Connection*>(ev.udata);
            destroy_connection(conn);
        }
        return;
    }

    if (ev.flags & EV_EOF) {
        if (ev.udata != &listener_tag_ && ev.udata != &timer_tag_) {
            auto* conn = static_cast<Connection*>(ev.udata);
            destroy_connection(conn);
        }
        return;
    }

    if (ev.udata == &listener_tag_) {
        handle_accept();
    } else if (ev.udata == &timer_tag_) {
        handle_timer();
    } else {
        auto* conn = static_cast<Connection*>(ev.udata);
        handle_connection_event(conn, ev);
    }
}

void EventLoop::handle_accept() noexcept
{
    for (;;) {
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);
        int fd = ::accept(listen_fd_,
                          reinterpret_cast<struct sockaddr*>(&addr),
                          &addrlen);

        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break; // no more connections
            break;
        }

        // Set non-blocking and TCP_NODELAY
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        auto* conn = create_connection(fd);
        if (!conn) {
            ::close(fd);
            break; // pool exhausted
        }
    }
}

void EventLoop::handle_timer() noexcept
{
    auto timeout = std::chrono::seconds(keep_alive_timeout_);

    for (auto& conn : pool_) {
        if (conn.state() == Connection::State::KEEP_ALIVE &&
            conn.is_expired(timeout))
        {
            destroy_connection(&conn);
        }
    }
}

void EventLoop::handle_connection_event(Connection* conn,
                                        const struct kevent& ev) noexcept
{
    process_connection(conn);
}

void EventLoop::process_connection(Connection* conn) noexcept
{
    if (conn->is_closed()) return;

    Connection::Want want = Connection::Want::NONE;

    switch (conn->state()) {
    case Connection::State::TLS_ACCEPT:
        want = conn->do_tls_accept();
        break;

    case Connection::State::READING:
        want = conn->do_read();
        break;

    case Connection::State::PARSING:
        want = conn->do_parse();
        break;

    case Connection::State::SENDING_HEADERS:
        want = conn->do_send_headers();
        break;

    case Connection::State::SENDING_BODY:
        want = conn->do_send_body();
        break;

    case Connection::State::KEEP_ALIVE: {
        conn->reset();
        want = conn->do_read();
        break;
    }

    default:
        break;
    }

    if (conn->is_closed()) {
        destroy_connection(conn);
        return;
    }

    // If parse succeeded and we have a valid request, prepare the response
    if ((conn->state() == Connection::State::PARSING ||
         conn->state() == Connection::State::READING) &&
        conn->request().method() != HttpRequest::Method::UNKNOWN)
    {
        prepare_response(conn);
        want = conn->do_send_headers();
    }

    update_events(conn, want);
}

void EventLoop::prepare_response(Connection* conn) noexcept
{
    auto& req  = conn->request();
    auto& resp = conn->response();
    resp.reset();

    bool keep_alive = req.is_keep_alive();
    resp.set_keep_alive(keep_alive);

    // Validate method
    if (req.method() != HttpRequest::Method::GET &&
        req.method() != HttpRequest::Method::HEAD)
    {
        resp.set_status(HttpResponse::Status::NOT_IMPLEMENTED);
        auto body = HttpResponse::error_body(HttpResponse::Status::NOT_IMPLEMENTED);
        resp.set_body(std::move(body));
        resp.set_content_type("text/html; charset=utf-8");
        resp.set_content_length(resp.body().size());
        conn->prepare_headers();
        return;
    }

    // Open file
    auto result = file_server_.open(req.path());
    if (auto* err = std::get_if<FileError>(&result)) {
        HttpResponse::Status st;
        switch (*err) {
        case FileError::NOT_FOUND:   st = HttpResponse::Status::NOT_FOUND; break;
        case FileError::FORBIDDEN:   st = HttpResponse::Status::FORBIDDEN; break;
        case FileError::BAD_REQUEST: st = HttpResponse::Status::BAD_REQUEST; break;
        default:                     st = HttpResponse::Status::INTERNAL_ERROR; break;
        }
        resp.set_status(st);
        auto body = HttpResponse::error_body(st);
        resp.set_body(std::move(body));
        resp.set_content_type("text/html; charset=utf-8");
        resp.set_content_length(resp.body().size());
        conn->prepare_headers();
        return;
    }

    auto& info = std::get<FileInfo>(result);

    // Check If-Modified-Since
    auto ims = req.parse_if_modified_since();
    if (ims && *ims >= info.last_modified) {
        resp.set_status(HttpResponse::Status::NOT_MODIFIED);
        resp.set_last_modified(info.last_modified);
        conn->prepare_headers();
        // info destructor closes fd
        return;
    }

    // Check Range
    off_t range_start = 0;
    off_t range_end = info.size - 1;
    auto range = req.parse_range(info.size);

    if (range) {
        range_start = range->first;
        range_end = range->second;
        resp.set_status(HttpResponse::Status::PARTIAL_CONTENT);
        std::string cr = "bytes " +
            std::to_string(range_start) + "-" +
            std::to_string(range_end) + "/" +
            std::to_string(info.size);
        resp.set_header("Content-Range", std::move(cr));
    }

    off_t content_length = (info.size > 0) ? (range_end - range_start + 1) : 0;

    resp.set_status(range ? HttpResponse::Status::PARTIAL_CONTENT
                          : HttpResponse::Status::OK);
    resp.set_content_type(info.mime_type);
    resp.set_content_length(static_cast<uint64_t>(content_length));
    resp.set_last_modified(info.last_modified);
    resp.set_accept_ranges(true);

    conn->prepare_headers();
    conn->set_send_file(info.release_fd(), info.size,
                        range_start, range_end,
                        req.method() == HttpRequest::Method::HEAD);
}

void EventLoop::update_events(Connection* conn, Connection::Want want) noexcept
{
    int fd = conn->fd();
    if (fd < 0 || conn->is_closed()) return;

    switch (want) {
    case Connection::Want::READ:
        add_event(static_cast<uintptr_t>(fd), EVFILT_READ,
                  EV_ADD | EV_ENABLE, conn);
        add_event(static_cast<uintptr_t>(fd), EVFILT_WRITE,
                  EV_DELETE, nullptr);
        break;

    case Connection::Want::WRITE:
        add_event(static_cast<uintptr_t>(fd), EVFILT_READ,
                  EV_DELETE, nullptr);
        add_event(static_cast<uintptr_t>(fd), EVFILT_WRITE,
                  EV_ADD | EV_ENABLE, conn);
        break;

    case Connection::Want::NONE:
    default:
        break;
    }
}

Connection* EventLoop::create_connection(int fd) noexcept
{
    auto* conn = pool_.acquire();
    if (!conn) return nullptr;

    SSL* ssl = nullptr;
    if (tls_ctx_) {
        ssl = tls_ctx_->new_ssl();
        if (ssl)
            SSL_set_fd(ssl, fd);
    }

    conn->init(fd, ssl, file_server_.root_dir(), keep_alive_timeout_);

    // Register for read events
    add_event(static_cast<uintptr_t>(fd), EVFILT_READ,
              EV_ADD | EV_ENABLE, conn);

    return conn;
}

void EventLoop::destroy_connection(Connection* conn) noexcept
{
    int fd = conn->fd();
    if (fd >= 0) {
        // Remove from kqueue
        add_event(static_cast<uintptr_t>(fd), EVFILT_READ,
                  EV_DELETE, nullptr);
        add_event(static_cast<uintptr_t>(fd), EVFILT_WRITE,
                  EV_DELETE, nullptr);
    }
    conn->close();
    pool_.release(conn);
}
