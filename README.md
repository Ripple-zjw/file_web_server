# file_web_server

基于 kqueue 的单线程事件驱动 HTTP/1.1 文件服务器，支持可选 TLS。仅支持 macOS（使用了 `kqueue`、`sendfile`、`timegm` 等平台特定 API）。

## 特性

- **事件驱动** — 单线程 + kqueue 事件循环，固定连接池（16384 槽位），无动态连接分配
- **状态机设计** — 每个连接由 `CLOSED → PROTO_DETECT → TLS_ACCEPT → READING → PARSING → SENDING_HEADERS → SENDING_BODY → KEEP_ALIVE` 状态机驱动，配合 `Want::{READ, WRITE, NONE}` 返回值实现非阻塞 I/O 断点续传
- **协议动态检测** — 同一端口同时支持 HTTP 和 HTTPS（通过首个字节判断）
- **TLS 支持** — 通过 OpenSSL 提供可选加密，TLS 下退化为 `pread + SSL_write` 分块发送
- **零拷贝文件发送** — 非 TLS 下使用 `sendfile()` 系统调用
- **HTTP 特性** — 支持 Range 请求（单区间）、If-Modified-Since、HEAD 方法、Keep-Alive 连接复用
- **安全文件服务** — 路径规范化拒绝 `..` 逃逸、点文件隐藏、空字节注入；符号链接解析后验证仍在根目录内；目录请求自动查找 `index.html`
- **55 种 MIME 类型** — 覆盖常见文件扩展名

## 构建

### 依赖

- macOS（仅支持）
- CMake ≥ 3.20
- OpenSSL（`libssl`、`libcrypto`）
- C++20 编译器（Apple Clang）

### 编译

```sh
cmake -S . -B build
cmake --build build
```

默认编译 Debug 版本，并生成 `compile_commands.json`（供 clangd 使用）。

## 使用

```sh
# 启动 HTTP 服务器，服务当前目录
./build/web_server

# 指定根目录和端口
./build/web_server --root /path/to/serve --port 8080

# 启动 HTTPS（需要先生成证书）
./cert/gen_cert.sh
./build/web_server --root . --cert cert/cert.pem --key cert/key.pem

# 设置 Keep-Alive 超时（默认 75 秒）
./build/web_server --root . --port 3000 --timeout 120
```

### 命令行选项

| 选项 | 说明 | 默认值 |
|------|------|--------|
| `--root` | 服务根目录 | `.` |
| `--port` | 监听端口 | `8080` |
| `--timeout` | Keep-Alive 超时秒数 | `75` |
| `--cert` | TLS 证书路径 | （不启用 TLS） |
| `--key` | TLS 私钥路径 | （不启用 TLS） |
| `--help` / `-h` | 显示帮助 | — |

## 架构

```
┌─────────────────────────────────────────────────┐
│                    main.cpp                      │
│   创建监听 socket → 传入 EventLoop::run()        │
└──────────────────┬──────────────────────────────┘
                   │
┌──────────────────▼──────────────────────────────┐
│             EventLoop (kqueue)                   │
│  阻塞在 kevent() → 分发三种事件:                  │
│  • Listener → accept() → 分配 Connection         │
│  • Connection → 运行状态机                       │
│  • Timer (每秒) → 关闭超时 Keep-Alive 连接       │
└──────────────────┬──────────────────────────────┘
                   │
┌──────────────────▼──────────────────────────────┐
│       Connection 状态机                          │
│                                                 │
│  CLOSED → PROTO_DETECT → TLS_ACCEPT → READING   │
│  READING → PARSING → SENDING_HEADERS            │
│  SENDING_HEADERS → SENDING_BODY → KEEP_ALIVE    │
│  KEEP_ALIVE → READING (下一个请求)              │
└──────────────────┬──────────────────────────────┘
                   │
┌──────────────────▼──────────────────────────────┐
│     HTTP 解析 & 响应构建                          │
│  • http_request：请求行 + 头部解析               │
│  • http_response：响应构建 + sendfile 发送        │
│  • file_server：路径安全、文件服务、index.html     │
└─────────────────────────────────────────────────┘
```

### 目录结构

```
src/
├── main.cpp              # 入口：监听 socket + 启动事件循环
├── core/
│   ├── event_loop.cpp/h  # kqueue 事件循环调度
│   ├── connection.cpp/h  # 连接状态机实现
│   ├── connection_pool.h # 固定大小连接池 (16384)
│   ├── tls_context.h     # OpenSSL 上下文封装
│   └── debug_log.h       # Debug 日志宏
├── http/
│   ├── http_request.cpp/h # HTTP 请求解析
│   ├── http_response.cpp/h# HTTP 响应构建 & 发送
│   └── mime_types.h       # MIME 类型表 (55 种)
└── server/
    ├── cli.h              # 命令行参数解析
    └── file_server.cpp/h  # 文件服务逻辑
```

## 状态机设计

单线程事件驱动的核心挑战是：每次回调只能做"当前能做的一小步"，做完就要返回。状态机就是在返回后记住进度的机制。

| 状态 | 做的工作 | 完成后去哪 |
|------|----------|-----------|
| PROTO_DETECT | peek 首字节判协议 | TLS_ACCEPT 或 READING |
| READING | 读数据 | PARSING（数据够了）|
| PARSING | 解析 HTTP 请求 | SENDING_HEADERS |
| SENDING_HEADERS | 发响应头 | SENDING_BODY 或 KEEP_ALIVE |
| SENDING_BODY | sendfile 发文件体 | KEEP_ALIVE（发完了）|
| KEEP_ALIVE | 等下一个请求 | 回到 READING |

每个状态返回的 `Want` 值告知事件循环需要注册什么事件：

| Want | 含义 | 事件循环做什么 |
|------|------|--------------|
| READ | 在等数据到来 | 注册 `EVFILT_READ`，数据到了再叫我 |
| WRITE | 在等 socket 可写 | 注册 `EVFILT_WRITE`，缓冲区有空间了再叫我 |
| NONE | 内部已转状态 | 当前回合继续往下走 |

## 协议

- **HTTP/1.1** — 仅支持
- **方法** — `GET`、`HEAD`
- **TLS** — 最低 TLS 1.2，HIGH 密码套件，禁止压缩

## 许可证

本项目基于 MIT 许可证。详见 [LICENSE](LICENSE) 文件。
