# 目录列表功能设计文档

## 概述

为基于 kqueue 的单线程 HTTP 文件服务器添加目录列表功能。当浏览器请求一个目录且该目录中没有 `index.html` 时，生成一个美观的 HTML 文件列表页面，支持点击下载和导航。

## 改动范围

仅修改 3 个文件：

| 文件 | 改动 |
|------|------|
| `src/server/file_server.h` | 新增 `DirEntry` 结构体、`FileError::IS_DIRECTORY` 枚举、`list_directory()` 方法声明、`build_directory_html()` 静态方法声明 |
| `src/server/file_server.cpp` | 实现目录列表读取、排序、HTML 生成全流程；修改 `open()` 中目录响应逻辑 |
| `src/core/event_loop.cpp` | 在 `prepare_response()` 中处理 `IS_DIRECTORY` 结果 |

不新增文件，不修改核心事件循环和连接状态机。

## 数据流

```
浏览器请求 GET /docs/
  → do_read() → do_parse()                    // 读取并解析请求
  → prepare_response()
    → file_server_.open("/docs/")             // 尝试打开文件
      → 检测到目录，无 index.html
      → 返回 FileError::IS_DIRECTORY          // 不再是 403，而是"这是一个可列目录"
    → 调用 file_server_.list_directory("/docs/")  // 获取条目
    → 调用 file_server_.build_directory_html(...)  // 生成 HTML
    → 作为 200 OK 响应返回
  → do_send_headers() → do_send_body() → done
```

## 详细设计

### 1. `DirEntry` 结构体（file_server.h）

```cpp
struct DirEntry {
    std::string name;          // 文件名（UTF-8 安全）
    bool        is_directory;  // 是否为目录
    off_t       size;          // 文件大小（字节）
    std::time_t last_modified; // 最后修改时间（Unix 时间戳）
};
```

### 2. 新增 `FileError::IS_DIRECTORY` 枚举

在 `FileError` 中新增一个值，区别于 `FORBIDDEN`（真正的权限拒绝）：
- `NOT_FOUND` — 文件不存在
- `FORBIDDEN` — 符号链接逃逸、特殊文件（设备/FIFO 等）、隐藏文件
- `BAD_REQUEST` — 路径含非法字符
- `IS_DIRECTORY` — 目录且无 index.html，应列出

### 3. `FileServer::open()` 修改

当前代码在 `S_ISDIR(st.st_mode)` 分支中，如果找不到 `index.html` 就返回 `FORBIDDEN`。改为返回 `IS_DIRECTORY`，表明"这是目录，需要列表"。

### 4. `FileServer::list_directory()`（file_server.cpp）

实现步骤：
1. 规范化请求路径（调用 `normalize()`，防 `..` 逃逸等）
2. 拼接根目录得到完整路径
3. `opendir()` 打开目录
4. `readdir()` 循环读取条目：
   - 跳过 `.`、`..`、以 `.` 开头的隐藏文件
   - 跳过目录自身（与 `normalize()` 策略一致，保护隐藏内容）
5. `stat()` 每个条目获取类型、大小、最后修改时间
6. 排序：目录在前，文件在后；各自按字母序升序
7. 返回 `std::vector<DirEntry>`

### 5. `FileServer::build_directory_html()` 生成 HTML 列表页

静态方法，签名：
```cpp
static std::string build_directory_html(
    std::string_view request_path,
    const std::vector<DirEntry>& entries,
    bool show_parent_link  // 是否显示 ../（根目录不显示）
);
```

#### HTML 结构

```html
<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Index of /path</title>
  <style>
    /* 美观的深色主题，响应式，清晰排版 */
  </style>
</head>
<body>
  <div class="container">
    <h1>📁 Index of /path</h1>
    <table>
      <thead>
        <tr><th>Name</th><th>Size</th><th>Modified</th></tr>
      </thead>
      <tbody>
        <!-- 父目录链接（非根目录时） -->
        <tr class="parent">
          <td><a href="../">📂 ../</a></td>
          <td class="size">-</td>
          <td class="date">-</td>
        </tr>
        <!-- 文件条目，循环生成 -->
        <tr>
          <td><a href="filename">📄 filename</a></td>
          <td class="size">1.5 MB</td>
          <td class="date">2026-07-19 14:30</td>
        </tr>
      </tbody>
    </table>
    <footer>web_server_cpp/1.0</footer>
  </div>
</body>
</html>
```

#### 大小格式化

- < 1 KB → 精确到字节显示 "xxx B"
- < 1 MB → 小数点后 1 位显示 "xx.x KB"
- < 1 GB → 小数点后 1 位显示 "xx.x MB"
- ≥ 1 GB → 小数点后 1 位显示 "xx.x GB"

#### 时间格式化

使用友好格式：`YYYY-MM-DD HH:MM`（本地时间，从 `last_modified` 的 UTC 时间戳转换）。

#### 安全性

- 文件名中的特殊字符（`<`、`>`、`&`、`"`、`'`）进行 HTML 转义
- 文件名中的 URL 特殊字符（`%`、`#`、`?`、` `）进行 URL 编码
- 隐藏文件一概不显示

### 6. `EventLoop::prepare_response()` 修改

新增一个 `IS_DIRECTORY` 分支：

```cpp
case FileError::IS_DIRECTORY: {
    auto entries = file_server_.list_directory(req.path());
    bool show_parent = (req.path() != "/");  // 根目录不显示父目录
    auto html = FileServer::build_directory_html(req.path(), entries, show_parent);
    resp.set_status(HttpResponse::Status::OK);
    resp.set_body(std::move(html));
    resp.set_content_type("text/html; charset=utf-8");
    resp.set_content_length(resp.body().size());
    conn->prepare_headers();
    return;
}
```

注意 `file_server_.list_directory()` 内部已经调用了 `normalize()`，所以在这个分支中不需要再担心路径安全问题。

### 7. 隐藏文件处理

`open()` 中的 `normalize()` 已经拒绝以 `.` 开头的路径段（步骤 5），这意味着请求 `/.hidden_file` 会返回 `BAD_REQUEST`。但是对于目录列表本身：
- `list_directory()` 使用 `readdir()` 读取后手动跳过以 `.` 开头的条目
- 这包括 `.hidden_file` 和 `.hidden_directory` 等

### 8. 父目录导航安全

父目录链接的实现：
- HTML 中渲染 `<a href="../">📂 ../</a>` 链接
- 当用户点击时，浏览器发送 `GET ../` 请求
- `FileServer::normalize()` 会解析 `..` 路径段：如果已经在根级别（parts 为空）则返回空字符串（`BAD_REQUEST`），不会越过根目录
- 所以安全性由现有的 `normalize()` 机制保证

对于根目录 `/`：
- `show_parent_link = false`，不渲染 `../` 链接
- 用户无法通过目录列表导航到根目录之外

### 9. 视觉设计

采用深色主题（适配当前 macOS 系统的暗色模式偏好），设计风格：

- **配色**：深色背景 (`#1a1a2e`)，浅色文字 (`#e0e0e0`)，品牌色强调 (`#4a9eff`)
- **排版**：系统无衬线字体（`-apple-system, BlinkMacSystemFont, "Segoe UI"`）
- **表格**：悬停高亮，柔和的分隔线，图标前缀（📂目录、📄文件、📁父目录）
- **响应式**：`viewport` 适配，小屏幕下自动缩放
- **简洁**：去除多余装饰，聚焦于内容

## 不变的部分

- 连接状态机不变
- 事件循环主流程不变
- 文件缓存/Cache-Control 逻辑不变
- TLS 处理不变
- 连接池管理不变

## 边界情况

| 场景 | 行为 |
|------|------|
| 根目录 `/` | 列出根目录所有文件，不显示 `../` 链接 |
| 空目录 | 显示空表格（仅表头和 `../` 链接） |
| 无读权限的目录 | `opendir` 失败，返回 403（通过 FORBIDDEN 分支） |
| 全是隐藏文件的目录 | 显示空表格 |
| 单个文件请求 | 保持不变，正常返回文件 |
| 有 index.html 的目录 | 保持不变，优先返回 index.html |
| 目录列表很大 | HTML 一次性构建，受限于响应体大小（与错误页面类似） |

## 未来扩展（不在此次实现中）

- 分页（大目录）
- 搜索/过滤
- 排序切换（按名称/大小/时间）
- 面包屑导航
- 目录列表缓存
