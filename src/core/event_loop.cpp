#include "event_loop.h"

#include "debug_log.h"
#include "http/http_response.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

/**
 * @brief 构造事件循环：创建 kqueue，初始化 FileServer。
 * @param root_dir           文件服务的根目录
 * @param keep_alive_timeout Keep-Alive 超时秒数
 */
EventLoop::EventLoop(std::string root_dir, int keep_alive_timeout) noexcept
    : file_server_(std::move(root_dir))
    , keep_alive_timeout_(keep_alive_timeout)
{
    kq_ = kqueue();
    DEBUG_LOG("kq=%d", kq_);
}

/// 析构：关闭 kqueue 文件描述符
EventLoop::~EventLoop() noexcept
{
    DEBUG_LOG("close kq=%d", kq_);
    if (kq_ >= 0) ::close(kq_);
}

/**
 * @brief 向 changelist_ 添加一条 kqueue 修改事件。
 *
 * 注意：EV_SET 宏中的 fflags 和 data 参数被硬编码为 0，
 * 不适合需要自定义定时器参数的场景（使用方式见 add_listener 中的定时器注册）。
 *
 * @param ident  事件标识（通常为 fd）
 * @param filter 事件过滤器类型
 * @param flags  操作标志位
 * @param udata  附带的用户数据指针
 */
void EventLoop::add_event(uintptr_t ident, int16_t filter, uint16_t flags,
                          void* udata) noexcept
{
    struct kevent ev;
    EV_SET(&ev, ident, filter, flags, 0, 0, udata);
    changelist_.push_back(ev);
}

/// 将 changelist_ 中积攒的所有修改一次性提交到 kqueue，然后清空。
void EventLoop::flush_changelist() noexcept
{
    if (changelist_.empty()) return;
    DEBUG_LOG("submit %zu changes", changelist_.size());
    kevent(kq_, changelist_.data(), static_cast<int>(changelist_.size()),
           nullptr, 0, nullptr);
    changelist_.clear();
}

/**
 * @brief 注册监听 socket 和 keep-alive 定时器。
 *
 * 将 fd 设为非阻塞后，注册 EVFILT_READ 事件用于接收新连接。
 * 同时注册每秒触发的 EVFILT_TIMER 用于清理超时的 keep-alive 连接。
 *
 * @param fd 已处于监听状态的 socket 文件描述符
 */
void EventLoop::add_listener(int fd) noexcept
{
    listen_fd_ = fd;

    // 设为非阻塞模式
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    DEBUG_LOG("listen_fd=%d ka_timeout=%d", fd, keep_alive_timeout_);

    // 注册监听 socket 的读事件（新连接到达时触发）
    add_event(static_cast<uintptr_t>(fd), EVFILT_READ, EV_ADD | EV_ENABLE,
              &listener_tag_);

    // 注册 1 秒周期性定时器，用于 keep-alive 超时扫描
    {
        struct kevent tev;
        EV_SET(&tev, 1, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_CLEAR,
               NOTE_SECONDS, 1, &timer_tag_);
        changelist_.push_back(tev);
    }

    flush_changelist();
}

/**
 * @brief 事件循环主循环：阻塞在 kevent() 上，轮询处理事件直到收到退出信号。
 *
 * 每轮迭代：
 * 1. 提交 changelist 中积攒的修改
 * 2. 阻塞等待事件（被 EINTR 中断时安全重试）
 * 3. 清空 changelist
 * 4. 遍历返回的事件数组，逐个 dispatch
 */
void EventLoop::run() noexcept
{
    DEBUG_LOG("event loop started");

    // 忽略 SIGPIPE（写入已关闭的 socket 时避免进程被终止）
    signal(SIGPIPE, SIG_IGN);

    // 注册 SIGINT/SIGTERM 处理器，不使用 SA_RESTART 以让 kevent 返回 EINTR
    /*
     * 这段代码的作用是设置一个信号处理器，当程序收到 SIGINT 或 SIGTERM 信号时，会调用 handle_signal 函数，
     * 将 g_running 设置为 false，从而让事件循环退出。
     * 通过不使用 SA_RESTART 标志，kevent 调用在收到信号时会返回 EINTR，
     * 这样我们就可以在下一次循环中检查 g_running 并安全地退出事件循环。 
     * 为什么要做这个？因为我们希望在收到终止信号时能够立即退出事件循环，
     * 而不是被阻塞在 kevent 调用中。通过不使用 SA_RESTART，
     * kevent 会在信号到来时返回 EINTR，这样我们就可以检查 g_running 标志并安全地退出循环。
     */
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
            if (errno == EINTR) continue; // 被信号中断，重新进入循环检查 g_running
            break;                          // 其他错误，退出
        }

        DEBUG_LOG("kevent returned %d events", n);
        for (int i = 0; i < n; ++i)
            dispatch(events_[i]);
    }

    DEBUG_LOG("event loop stopped");
}

/**
 * @brief 分发 kevent 事件到对应的处理函数。
 *
 * 分发逻辑（按优先级）：
 * - EV_ERROR：socket 错误 → 关闭连接（监听 socket 和定时器的事件静默忽略）
 * - EV_EOF：对端关闭 → 关闭连接
 * - udata 指向 listener_tag_ → 处理新连接 accept
 * - udata 指向 timer_tag_ → 处理超时扫描
 * - 其他 → 视为连接事件，进入 process_connection
 *
 * @param ev kevent 事件结构体
 */
void EventLoop::dispatch(const struct kevent& ev) noexcept
{
    if (ev.flags & EV_ERROR) {
        DEBUG_LOG("EV_ERROR ident=%lu filter=%d", ev.ident, ev.filter);
        // fd 上的错误事件 —— 关闭对应连接
        // 注意：EV_DELETE 操作失败的返回事件 udata 可能为 nullptr
        if (ev.udata != &listener_tag_ && ev.udata != &timer_tag_ &&
            ev.udata != nullptr) {
            auto* conn = static_cast<Connection*>(ev.udata);
            destroy_connection(conn);
        }
        return;
    }

    if (ev.flags & EV_EOF) {
        // 对端关闭连接
        if (ev.udata != &listener_tag_ && ev.udata != &timer_tag_ &&
            ev.udata != nullptr) {
            auto* conn = static_cast<Connection*>(ev.udata);
            DEBUG_LOG("EV_EOF conn=%p fd=%d", (void*)conn, conn->fd());
            destroy_connection(conn);
        }
        return;
    }

    if (ev.udata == &listener_tag_) {
        DEBUG_LOG("listener event");
        handle_accept();
    } else if (ev.udata == &timer_tag_) {
        handle_timer();
    } else {
        auto* conn = static_cast<Connection*>(ev.udata);
        handle_connection_event(conn, ev);
    }
}

/**
 * @brief 处理新连接：循环 accept 直到队列耗尽或连接池满。
 *
 * 每个 accept 成功的连接：
 * 1. 设为非阻塞模式并启用 TCP_NODELAY
 * 2. 从连接池中分配 Connection 对象
 * 3. 若池满则关闭该 fd 并停止 accept
 */
void EventLoop::handle_accept() noexcept
{
    int accepted = 0;
    for (;;) {
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);
        int fd = ::accept(listen_fd_,
                          reinterpret_cast<struct sockaddr*>(&addr),
                          &addrlen);

        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break; // 所有等待的连接已处理完毕
            break;
        }
        ++accepted;

        // 设为非阻塞，并关闭 Nagle 算法以降低延迟
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        auto* conn = create_connection(fd);
        if (!conn) {
            DEBUG_LOG("connection pool full, close fd=%d", fd);
            ::close(fd);
            break; // 连接池已满，停止接受
        }
    }
    DEBUG_LOG_IF(accepted > 0, "accepted %d connections, active=%zu",
                 accepted, pool_.active_count());
}

/**
 * @brief 处理定时器事件：遍历 keep-alive 链表，关闭超时的连接。
 *
 * 只遍历处于 KEEP_ALIVE 状态的连接（已在链表中），
 * 根据 last_active_ 时间戳判断是否超时。
 * 使用侵入式链表避免扫描 16384 个池槽位。
 */
void EventLoop::handle_timer() noexcept
{
    auto timeout = std::chrono::seconds(keep_alive_timeout_);
    int expired = 0;

    Connection* conn = keepalive_head_;
    while (conn) {
        Connection* next = conn->keepalive_next_; // 先存下个指针——destroy 后 conn 失效
        if (conn->is_expired(timeout)) {
            ++expired;
            destroy_connection(conn);
        }
        conn = next;
    }

    DEBUG_LOG_IF(expired > 0, "expired %d keep-alive connections, active=%zu",
                 expired, pool_.active_count());
}

/**
 * @brief 连接事件入口，将控制流传入 process_connection。
 *
 * @note @p ev 参数在当前实现中未使用（未来可用于区分读/写触发类型），
 *       所有连接事件统一交给状态机自行判断当前应执行的动作。
 *
 * @param conn 触发事件的连接对象
 * @param ev   kevent 事件结构体（当前未使用）
 */
void EventLoop::handle_connection_event(Connection* conn,
                                        const struct kevent&) noexcept
{
    process_connection(conn);
}

/**
 * @brief 运行连接状态机 —— 根据当前状态执行对应的动作，串联读→解析→响应→发送的完整流程。
 *
 * 流程：
 * 1. 根据 conn->state() 分发执行对应动作（do_tls_accept/do_read/do_parse/...）
 * 2. 若 do_read 刚刚完成（状态转入 PARSING），立即内联执行 do_parse，无需等待新事件
 * 3. 若解析完成（method 不再为 UNKNOWN），调用 prepare_response 构建响应并开始发送
 * 4. 根据最终 Want 值更新 kqueue 事件注册
 *
 * @param conn 连接对象
 */
void EventLoop::process_connection(Connection* conn) noexcept
{
    if (conn->is_closed()) return;

    // 记录状态机入口状态，用于后续对比是否进出 KEEP_ALIVE
    auto prev_state = conn->state();

    Connection::Want want = Connection::Want::NONE;

    switch (conn->state()) {
    case Connection::State::PROTO_DETECT:
        want = conn->do_proto_detect(tls_ctx_ != nullptr);
        break;

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

    // PROTO_DETECT 检测到 TLS → 注入 SSL 并立即执行握手
    if (conn->state() == Connection::State::TLS_ACCEPT && !conn->is_tls()) {
        SSL* ssl = tls_ctx_ ? tls_ctx_->new_ssl() : nullptr;
        if (ssl) {
            SSL_set_fd(ssl, conn->fd());
            conn->set_ssl(ssl);
        }
        want = conn->do_tls_accept();
    }

    DEBUG_LOG("state=%d want=%d fd=%d",
              static_cast<int>(conn->state()),
              static_cast<int>(want), conn->fd());

    if (conn->is_closed()) {
        destroy_connection(conn);
        return;
    }

    // 若 do_read 已完成头部读取并转入 PARSING 状态，则立即解析
    if (conn->state() == Connection::State::PARSING &&
        conn->request().method() == HttpRequest::Method::UNKNOWN)
    {
        DEBUG_LOG("inline parse fd=%d", conn->fd());
        want = conn->do_parse();
    }

    // 解析已完成 → 构建响应并开始发送
    if ((conn->state() == Connection::State::PARSING ||
         conn->state() == Connection::State::READING) &&
        conn->request().method() != HttpRequest::Method::UNKNOWN)
    {
        DEBUG_LOG("prepare response fd=%d path=%.*s",
                  conn->fd(),
                  static_cast<int>(conn->request().path().size()),
                  conn->request().path().data());
        prepare_response(conn);
        want = conn->do_send_headers();
    }

    // 对比状态机运行前后的状态，同步 Keep-Alive 链表
    if (prev_state != Connection::State::KEEP_ALIVE &&
        conn->state() == Connection::State::KEEP_ALIVE) {
        add_to_keepalive_list(conn);
    } else if (prev_state == Connection::State::KEEP_ALIVE &&
               conn->state() != Connection::State::KEEP_ALIVE) {
        remove_from_keepalive_list(conn);
    }

    update_events(conn, want);
}

/**
 * @brief 根据当前请求准备 HTTP 响应 —— 验证方法、打开文件、检查缓存和 Range。
 *
 * 处理顺序：
 * 1. 只允许 GET/HEAD 方法，否则返回 501
 * 2. 通过 FileServer 打开请求路径对应的文件
 * 3. 若文件不存在/禁止访问/路径非法，返回对应错误
 * 4. 检查 If-Modified-Since → 命中则返回 304
 * 5. 处理 Range 头部 → 设置 206 Partial Content
 * 6. 设置响应状态码、各标准头部、文件发送信息
 *
 * @param conn 连接对象，从中读取请求并向其写入响应配置
 */
void EventLoop::prepare_response(Connection* conn) noexcept
{
    auto& req  = conn->request();
    auto& resp = conn->response();
    resp.reset();

    bool keep_alive = req.is_keep_alive();
    resp.set_keep_alive(keep_alive);

    bool is_head = (req.method() == HttpRequest::Method::HEAD);
    conn->set_is_head(is_head);

    // 验证方法 —— 仅支持 GET 和 HEAD
    if (req.method() != HttpRequest::Method::GET &&
        req.method() != HttpRequest::Method::HEAD)
    {
        DEBUG_LOG("unsupported method fd=%d method=%.*s",
                  conn->fd(),
                  static_cast<int>(req.method_str().size()),
                  req.method_str().data());
        resp.set_status(HttpResponse::Status::NOT_IMPLEMENTED);
        auto body = HttpResponse::error_body(HttpResponse::Status::NOT_IMPLEMENTED);
        resp.set_body(std::move(body));
        resp.set_content_type("text/html; charset=utf-8");
        resp.set_content_length(resp.body().size());
        conn->prepare_headers();
        DEBUG_LOG("=> %d %.*s %.*s body=%.*s",
                  static_cast<int>(resp.status()),
                  static_cast<int>(req.method_str().size()),
                  req.method_str().data(),
                  static_cast<int>(req.path().size()),
                  req.path().data(),
                  static_cast<int>(resp.body().size() < 256 ? resp.body().size() : 256),
                  resp.body().data());
        return;
    }

    // 打开请求文件
    auto result = file_server_.open(req.path());
    if (auto* err = std::get_if<FileError>(&result)) {
        // IS_DIRECTORY：生成目录列表页面
        if (*err == FileError::IS_DIRECTORY) {
            auto norm = FileServer::normalize(req.path());
            bool show_parent = (norm != "/");
            auto entries = file_server_.list_directory(req.path());
            auto html = FileServer::build_directory_html(req.path(), entries, show_parent);
            DEBUG_LOG("directory listing fd=%d path=%.*s entries=%zu",
                      conn->fd(),
                      static_cast<int>(req.path().size()), req.path().data(),
                      entries.size());
            resp.set_status(HttpResponse::Status::OK);
            resp.set_body(std::move(html));
            resp.set_content_type("text/html; charset=utf-8");
            resp.set_content_length(resp.body().size());
            conn->prepare_headers();
            return;
        }

        HttpResponse::Status st;
        switch (*err) {
        case FileError::NOT_FOUND:   st = HttpResponse::Status::NOT_FOUND; break;
        case FileError::FORBIDDEN:   st = HttpResponse::Status::FORBIDDEN; break;
        case FileError::BAD_REQUEST: st = HttpResponse::Status::BAD_REQUEST; break;
        default:                     st = HttpResponse::Status::INTERNAL_ERROR; break;
        }
        DEBUG_LOG("file error fd=%d err=%d status=%d",
                  conn->fd(), static_cast<int>(*err),
                  static_cast<int>(st));
        resp.set_status(st);
        auto body = HttpResponse::error_body(st);
        resp.set_body(std::move(body));
        resp.set_content_type("text/html; charset=utf-8");
        resp.set_content_length(resp.body().size());
        conn->prepare_headers();
        DEBUG_LOG("=> %d %.*s %.*s body=%.*s",
                  static_cast<int>(resp.status()),
                  static_cast<int>(req.method_str().size()),
                  req.method_str().data(),
                  static_cast<int>(req.path().size()),
                  req.path().data(),
                  static_cast<int>(resp.body().size() < 256 ? resp.body().size() : 256),
                  resp.body().data());
        return;
    }

    auto& info = std::get<FileInfo>(result);

    // 检查 If-Modified-Since 条件请求
    auto ims = req.parse_if_modified_since();
    if (ims && *ims >= info.last_modified) {
        DEBUG_LOG("304 fd=%d", conn->fd());
        resp.set_status(HttpResponse::Status::NOT_MODIFIED);
        resp.set_last_modified(info.last_modified);
        conn->prepare_headers();
        // info 析构时自动关闭 fd
        DEBUG_LOG("=> %d %.*s %.*s file=%s",
                  static_cast<int>(resp.status()),
                  static_cast<int>(req.method_str().size()),
                  req.method_str().data(),
                  static_cast<int>(req.path().size()),
                  req.path().data(),
                  info.resolved_path.c_str());
        return;
    }

    // 处理 Range 请求
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
    DEBUG_LOG("=> %d %.*s %.*s file=%s len=%lld",
              static_cast<int>(resp.status()),
              static_cast<int>(req.method_str().size()),
              req.method_str().data(),
              static_cast<int>(req.path().size()),
              req.path().data(),
              info.resolved_path.c_str(),
              (long long)content_length);
    resp.set_content_type(info.mime_type);
    resp.set_content_length(static_cast<uint64_t>(content_length));
    resp.set_last_modified(info.last_modified);
    resp.set_accept_ranges(true);

    conn->prepare_headers();
    conn->set_send_file(info.release_fd(), info.size,
                        range_start, range_end, is_head);
}

/**
 * @brief 根据 Want 状态注册或删除连接对应的 kqueue 事件。
 *
 * - Want::READ  → 注册 EVFILT_READ，删除 EVFILT_WRITE（等待可读）
 * - Want::WRITE → 注册 EVFILT_WRITE，删除 EVFILT_READ（等待可写）
 * - Want::NONE  → 不执行任何操作
 *
 * @param conn 连接对象
 * @param want 希望关注的事件方向
 */
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

/**
 * @brief 从连接池获取连接槽位，初始化后注册 kqueue 读事件。
 *
 * 若配置了 TLS，为连接创建 SSL 对象并关联到 socket fd。
 * 初始状态：TLS → TLS_ACCEPT，普通 → READING。
 *
 * @param fd 已 accept 的 socket fd
 * @return 已初始化的连接指针，池满或 SSL 创建失败时返回 nullptr
 */
Connection* EventLoop::create_connection(int fd) noexcept
{
    auto* conn = pool_.acquire();
    if (!conn) {
        DEBUG_LOG("pool full, fd=%d", fd);
        return nullptr;
    }

    conn->init(fd, file_server_.root_dir(), keep_alive_timeout_);
    conn->set_tls_available(tls_ctx_ != nullptr);

    DEBUG_LOG("fd=%d conn=%p", fd, (void*)conn);

    // 注册读事件以接收 HTTP 请求数据
    add_event(static_cast<uintptr_t>(fd), EVFILT_READ,
              EV_ADD | EV_ENABLE, conn);

    return conn;
}

// ---- Keep-Alive 链表操作实现 ----

void EventLoop::add_to_keepalive_list(Connection* conn) noexcept
{
    if (conn->in_keepalive_list_) return; // 已在链表中
    conn->in_keepalive_list_ = true;
    conn->keepalive_prev_ = nullptr;
    conn->keepalive_next_ = keepalive_head_;
    if (keepalive_head_)
        keepalive_head_->keepalive_prev_ = conn;
    keepalive_head_ = conn;
}

void EventLoop::remove_from_keepalive_list(Connection* conn) noexcept
{
    if (!conn->in_keepalive_list_) return; // 不在链表中
    conn->in_keepalive_list_ = false;
    if (conn->keepalive_prev_)
        conn->keepalive_prev_->keepalive_next_ = conn->keepalive_next_;
    else
        keepalive_head_ = conn->keepalive_next_;
    if (conn->keepalive_next_)
        conn->keepalive_next_->keepalive_prev_ = conn->keepalive_prev_;
    conn->keepalive_prev_ = nullptr;
    conn->keepalive_next_ = nullptr;
}

/**
 * @brief 销毁连接：从 kqueue 移除事件 → 关闭 fd/TLS/文件 → 归还连接池槽位。
 *
 * 注意：本函数不负责关闭 fd（由 Connection::close() 内部处理），
 * 但主动从 kqueue 中删除该 fd 的所有事件注册。
 *
 * @param conn 要销毁的连接指针
 */
void EventLoop::destroy_connection(Connection* conn) noexcept
{
    // 先移除链表（幂等：不在链表中则无操作）
    remove_from_keepalive_list(conn);

    int fd = conn->fd();
    DEBUG_LOG("fd=%d conn=%p state=%d", fd, (void*)conn,
              static_cast<int>(conn->state()));
    if (fd >= 0) {
        // 从 kqueue 中移除所有事件监听
        add_event(static_cast<uintptr_t>(fd), EVFILT_READ,
                  EV_DELETE, nullptr);
        add_event(static_cast<uintptr_t>(fd), EVFILT_WRITE,
                  EV_DELETE, nullptr);
    }
    conn->close();
    pool_.release(conn);
}
