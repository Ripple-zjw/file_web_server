# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run
每次编译时都编译debug版本，并且要生成compile_commands.json

```sh
# 构建
cmake -S . -B build
cmake --build build

# 运行
./build/web_server --root /path/to/serve --port 8080

# TLS（先生成证书）
./cert/gen_cert.sh
./build/web_server --root . --cert cert/cert.pem --key cert/key.pem

# 全部选项：--root（默认 .），--port（默认 8080），--timeout（默认 75），
#           --cert/--key（TLS），--help/-h
```

项目目前没有测试套件和 linter 配置。

## 架构概览

基于 kqueue 的单线程事件循环 HTTP/1.1 文件服务器，依赖 OpenSSL 提供可选 TLS 支持。仅支持 macOS（使用了 `kqueue`、`sendfile`、`timegm` 等平台特定 API）。

### 目录分层

| 目录 | 职责 |
|------|------|
| `src/core/` | 事件循环、连接状态机、固定大小连接池、TLS 上下文封装 |
| `src/http/` | HTTP 请求解析器、响应构建器、55 种 MIME 类型表 |
| `src/server/` | CLI 参数解析、文件服务器（含路径规范化和符号链接保护） |

### 核心流程

`main.cpp` 创建监听 socket → 传递给 `EventLoop::run()` 阻塞在 `kevent()` 上。调度循环使用 changelist（批量化 kqueue 修改）并处理三种事件：

1. **Listener 事件** → `accept()` → 从连接池获取 `Connection` → 注册 `EVFILT_READ`
2. **Connection 事件** → 运行连接状态机
3. **Timer 事件**（每秒）→ 扫描并关闭超时的 keep-alive 连接

### 目录列表功能

当请求路径是目录且没有 `index.html` 时，`FileServer::open()` 返回 `FileError::IS_DIRECTORY`（而非原来的 403 Forbidden）。`EventLoop::prepare_response()` 检测到后调用：

1. `FileServer::list_directory()` — `opendir`/`readdir` 读取条目，过滤隐藏文件（`.` 开头）、按"目录在前文件在后"排序
2. `FileServer::build_directory_html()` — 生成深色主题响应式 HTML 页面，含文件图标、大小格式化、修改时间；根目录不显示 `../` 链接

安全性由 `normalize()` 的路径解析保证：点击 `../` 链接时，`FileServer::normalize()` 阻止路径逃逸。

### 连接状态机

```
CLOSED → TLS_ACCEPT → READING → PARSING → SENDING_HEADERS → SENDING_BODY → KEEP_ALIVE → (回到 READING)
```

每个状态对应一个 `do_*()` 方法，返回 `Want::{READ, WRITE, NONE}` 告知事件循环需要监听什么事件。解析完成后 `prepare_response()` 同步构建完整响应（查文件、生成头部），随后状态机依次发送头部和文件体。

这个状态机设计的核心原因只有一个：非阻塞 I/O 需要状态来记住"上次干到哪了"。

对比阻塞式服务器的写法：

// 阻塞式 —— 一个线程一个连接，用流程控制
void handle(conn) {
    read_request(conn);      // 阻塞等数据
    parse(conn);
    build_response(conn);
    send_headers(conn);      // 阻塞等可写
    send_body(conn);         // 阻塞等可写
}

每一行都是同步阻塞的，CPU 在线程睡眠上浪费了。换成单线程 + kqueue 事件驱动后，不能阻塞等任何东西——每次回调只能做"当前能做的那一小步"，做完就返回，让事件循环去处理其他连接。

状态机就是用来在返回后记住进度的：

状态          做的工作             完成后去哪
────────────────────────────────────────────
PROTO_DETECT   peek 首字节判协议    TLS_ACCEPT 或 READING
READING        读数据                PARSING（数据够了）
PARSING        解析 HTTP 请求        SENDING_HEADERS
SENDING_HEADERS 发响应头             SENDING_BODY 或 KEEP_ALIVE
SENDING_BODY   sendfile 发文件体     KEEP_ALIVE（发完了）
KEEP_ALIVE     等下一个请求          回到 READING

每个 do_*() 返回的 Want::{READ, WRITE, NONE} 告诉事件循环三件事：

┌───────┬────────────────────────────┬─────────────────────────────────────────┐
│ Want  │            含义             │             事件循环做什么                │
├───────┼────────────────────────────┼─────────────────────────────────────────┤
│ READ  │ 我在等数据到来                │ 注册 EVFILT_READ，数据到了再叫我           │
├───────┼────────────────────────────┼─────────────────────────────────────────┤
│ WRITE │ 我在等 socket 可写           │ 注册 EVFILT_WRITE，缓冲区有空间了再叫我     │
├───────┼────────────────────────────┼─────────────────────────────────────────┤
│ NONE  │ 我内部已经转状态了，别等了     │ 当前回合继续往下走（比如 inline parse）      │
└───────┴────────────────────────────┴──────────────────────────

为什么不能合并？ 因为每次 kqueue 事件触发的是连接上"可读"或"可写应哪个阶段。Want::READ 可能意味着"等待新请求的头部"或者"等待 TLS握手的数据"——状态就是用来区分这个的。

举个具体例子，SENDING_BODY 的过程中 sendfile 返回 EAGAIN（socket记住"发到了文件的第 839 个字节"，下次 EVFILT_WRITE
触发时从断点续传。没有 send_offset_ 和 send_remaining_ 这些状态

总结：状态机 = 事件驱动的"断点续传"机制。 每个状态保存了足够的上下文，让单线程能在成百上千个连接之间快速切换，每次只做一小步非阻塞操作。

### 关键设计

- **固定连接池** (`ConnectionPool<Connection>`, 16384 槽位) — 无动态连接分配
- **全非阻塞 I/O**，基于 kqueue 事件驱动
- **文件服务安全性**：路径规范化拒绝 `..` 逃逸、点文件、空字节；符号链接解析后验证仍在根目录内；目录请求自动查找 `index.html`，无 index.html 则生成目录列表
- **TLS**：可选，连接级 SSL 对象按需分配。TLS 下退化为 pread+SSL_write 分块发送（sendfile 不适用于 TLS）
- **非 TLS 用 `sendfile()`** 零拷贝发送文件体
- **HTTP Range**（单区间）、**If-Modified-Since**、**HEAD** 方法、keep-alive 均支持
- **头部预构建**为一个字符串一次性发送，体单独通过 sendfile 或 TLS 分块循环发送
- **百分号解码**：`normalize()` 中 `%XX` 解码全部非零字节（0x01-0xFF），支持 UTF-8 多字节路径（中文文件名等）。拒绝空字节（`%00`）和控制字符

### 发布流程

GitHub Actions 在推送 `v*` 标签时自动构建 Release 版本：

```bash
git tag v1.0.0                # 打好版本标签
git push origin v1.0.0        # 推送标签触发构建
```

构建产物 `web_server-macos-universal.tar.gz` 自动上传到对应 GitHub Release 页面。

### 依赖

- C++20, OpenSSL (`libssl`, `libcrypto`)
- macOS 专用 (kqueue, sendfile, `timegm`)
- CMake ≥ 3.20
