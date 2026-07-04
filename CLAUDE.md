# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run

```sh
# 构建
cmake -S . -B build
cmake --build build
每次编译时都编译debug版本，并且要生成compile_commands.json

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

### 连接状态机

```
CLOSED → TLS_ACCEPT → READING → PARSING → SENDING_HEADERS → SENDING_BODY → KEEP_ALIVE → (回到 READING)
```

每个状态对应一个 `do_*()` 方法，返回 `Want::{READ, WRITE, NONE}` 告知事件循环需要监听什么事件。解析完成后 `prepare_response()` 同步构建完整响应（查文件、生成头部），随后状态机依次发送头部和文件体。

### 关键设计

- **固定连接池** (`ConnectionPool<Connection>`, 16384 槽位) — 无动态连接分配
- **全非阻塞 I/O**，基于 kqueue 事件驱动
- **文件服务安全性**：路径规范化拒绝 `..` 逃逸、点文件、空字节；符号链接解析后验证仍在根目录内；目录请求自动查找 `index.html`
- **TLS**：可选，连接级 SSL 对象按需分配。TLS 下退化为 pread+SSL_write 分块发送（sendfile 不适用于 TLS）
- **非 TLS 用 `sendfile()`** 零拷贝发送文件体
- **HTTP Range**（单区间）、**If-Modified-Since**、**HEAD** 方法、keep-alive 均支持
- **头部预构建**为一个字符串一次性发送，体单独通过 sendfile 或 TLS 分块循环发送

### 依赖

- C++20, OpenSSL (`libssl`, `libcrypto`)
- macOS 专用 (kqueue, sendfile, `timegm`)
- CMake ≥ 3.20
