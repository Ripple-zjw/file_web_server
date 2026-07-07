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

/// 全局运行标志，由信号处理器设置为 0 以优雅退出
extern "C" {
inline std::sig_atomic_t g_running = 1;
/// 信号处理器：收到 SIGINT/SIGTERM 时将 g_running 置 0
inline void handle_signal(int) { g_running = 0; }
}

/**
 * @brief macOS kqueue 事件循环，是整个服务器的调度核心。
 *
 * 职责：
 * - 管理监听 socket 和所有活跃连接的 kqueue 事件
 * - 轮询 kevent 事件并分发到对应的处理函数
 * - 运行连接状态机，驱动请求读取 → 解析 → 响应准备 → 数据发送的流程
 * - 定时扫描并关闭超时的 keep-alive 连接
 *
 * 设计要点：
 * - 单线程运行，无需加锁
 * - 使用 changelist 批量提交 kqueue 修改（减少系统调用）
 * - 固定大小连接池，无动态内存分配
 */
class EventLoop {
public:
    /// kevent 返回事件数组的最大容量
    static constexpr int MAX_EVENTS = 1024;

    /**
     * @brief 构造事件循环。
     * @param root_dir           文件服务的根目录
     * @param keep_alive_timeout Keep-Alive 超时秒数
     */
    EventLoop(std::string root_dir, int keep_alive_timeout) noexcept;

    /// 析构时关闭 kqueue 描述符
    ~EventLoop() noexcept;

    /**
     * @brief 设置 TLS 上下文，后续新建连接将使用 TLS。
     * @param ctx TLS 上下文指针，为 nullptr 则不启用 TLS
     */
    void set_tls_context(TlsContext* ctx) noexcept { tls_ctx_ = ctx; }

    /**
     * @brief 注册监听 socket 到 kqueue，并启动 keep-alive 定时器。
     *
     * 将 fd 设为非阻塞模式，注册 EVFILT_READ 监听新连接，
     * 并注册一个每秒触发的 EVFILT_TIMER 用于清理过期连接。
     *
     * @param fd 已调用 listen 的 socket 文件描述符
     */
    void add_listener(int fd) noexcept;

    /**
     * @brief 启动事件循环主循环，阻塞直到收到 SIGINT/SIGTERM。
     *
     * 设置 SIGPIPE 忽略、注册信号处理器后进入 kevent 轮询循环。
     * 每轮：提交 changelist → 等待事件 → 清空 changelist → 分发事件。
     * EINTR 错误会安全重试，其他错误终止循环。
     */
    void run() noexcept;

private:
    using Pool = ConnectionPool<Connection>;

    int kq_ = -1;                        ///< kqueue 文件描述符
    int listen_fd_ = -1;                 ///< 监听 socket 的 fd
    TlsContext* tls_ctx_ = nullptr;      ///< TLS 上下文（可选）
    FileServer file_server_;             ///< 文件服务器实例
    Pool pool_;                          ///< 连接对象池
    int keep_alive_timeout_ = 75;        ///< Keep-Alive 超时秒数

    /// 侵入式 Keep-Alive 双向链表头（只包含处于 KEEP_ALIVE 状态的连接）
    Connection* keepalive_head_ = nullptr;

    /// kqueue 的事件 udata 标记，用于区分事件来源
    char listener_tag_ = 'L';            ///< 监听 socket 事件的标识
    char timer_tag_    = 'T';            ///< 定时器事件的标识

    /// kqueue 修改缓冲区，批量提交以减少系统调用
    std::vector<struct kevent> changelist_;

    /// kevent 返回的事件数组
    struct kevent events_[MAX_EVENTS];

    // ---- 内部辅助方法 ----

    /// 将 changelist_ 中积攒的修改一次性提交到 kqueue，然后清空
    void flush_changelist() noexcept;

    /**
     * @brief 向 changelist_ 添加一条 kqueue 修改。
     *
     * 注意：fflags 和 data 固定为 0，不适合需要自定义定时器参数的场景。
     *
     * @param ident  事件标识（通常是 fd）
     * @param filter 事件过滤器（EVFILT_READ/EVFILT_WRITE/EVFILT_TIMER）
     * @param flags  操作标志（EV_ADD/EV_DELETE/EV_ENABLE/EV_CLEAR）
     * @param udata  用户数据指针（用于事件分发时识别来源）
     */
    void add_event(uintptr_t ident, int16_t filter, uint16_t flags,
                   void* udata) noexcept;

    /**
     * @brief 根据事件来源（udata）将事件分发到对应处理函数。
     * @param ev kevent 事件结构体
     */
    void dispatch(const struct kevent& ev) noexcept;

    /// 处理新连接：循环 accept 直到 EAGAIN，每个连接分配一个 pool 槽位
    void handle_accept() noexcept;

    /// 处理定时器事件：扫描所有连接，关闭超时的 keep-alive 连接
    void handle_timer() noexcept;

    /**
     * @brief 处理单个连接的事件（将控制流转到 process_connection）。
     * @param conn 连接对象
     * @param ev   kevent 事件（当前未使用参数）
     */
    void handle_connection_event(Connection* conn,
                                 const struct kevent& ev) noexcept;

    /**
     * @brief 运行连接状态机，根据当前状态调用对应的动作函数。
     *
     * 流程：
     * 1. 根据 conn->state() 分发到 do_tls_accept/do_read/do_parse/...
     * 2. 若 do_read 转入 PARSING，立即内联调用 do_parse
     * 3. 若解析完成，调用 prepare_response 构建响应并开始发送
     * 4. 根据动作返回的 Want 更新 kqueue 事件注册
     *
     * @param conn 连接对象
     */
    void process_connection(Connection* conn) noexcept;

    /**
     * @brief 根据当前请求构建 HTTP 响应。
     *
     * 处理流程：验证方法 → 打开文件 → 检查 If-Modified-Since →
     * 处理 Range → 设置响应状态/头部/文件发送状态。
     *
     * @param conn 连接对象（从中读取请求，写入响应和文件发送状态）
     */
    void prepare_response(Connection* conn) noexcept;

    /**
     * @brief 根据连接的 Want 状态更新 kqueue 事件注册。
     *
     * Want::READ  → 注册 EVFILT_READ，删除 EVFILT_WRITE
     * Want::WRITE → 注册 EVFILT_WRITE，删除 EVFILT_READ
     * Want::NONE  → 不修改
     *
     * @param conn 连接对象
     * @param want 希望关注的事件方向
     */
    void update_events(Connection* conn, Connection::Want want) noexcept;

    /**
     * @brief 从连接池获取一个槽位并初始化连接。
     * @param fd  已 accept 的 socket fd
     * @return 初始化后的连接指针，池满返回 nullptr
     */
    Connection* create_connection(int fd) noexcept;

    /**
     * @brief 销毁连接：从 kqueue 移除事件 → 关闭 fd/SSL → 归还池槽位。
     * @param conn 要销毁的连接指针
     */
    void destroy_connection(Connection* conn) noexcept;

    // ---- Keep-Alive 链表操作（O(1)，侵入式双向链表） ----

    /// 将连接插入 keep-alive 链表头部（幂等：已在链表中则直接返回）
    void add_to_keepalive_list(Connection* conn) noexcept;

    /// 从 keep-alive 链表中移除连接（幂等：不在链表中则直接返回）
    void remove_from_keepalive_list(Connection* conn) noexcept;
};
