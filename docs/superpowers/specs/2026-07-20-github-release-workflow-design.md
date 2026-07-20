# GitHub Actions Release 构建工作流设计

## 概述

为 web_server 项目设置 GitHub Actions CI/CD，在推送版本标签时自动在 macOS ARM64 runner 上编译 Release 版本，并将产物上传到 GitHub Release。

## 触发条件

- 推送匹配 `v*` 的 tag 时触发（如 `v1.0.0`, `v2.3.1-beta`）
- 通过 `git tag v1.0.0 && git push origin v1.0.0` 触发

## 运行环境

- **Runner**: `macos-latest`（当前为 macOS 14 / Apple Silicon ARM64）
- **依赖**: OpenSSL（通过 Homebrew 安装 `openssl`）
- **编译器**: Apple Clang（macOS 自带）

## 构建配置

| 参数 | 值 | 说明 |
|------|-----|------|
| `CMAKE_BUILD_TYPE` | `Release` | 启用 `-O3 -flto -DNDEBUG`（沿用 CMakeLists.txt 已有配置） |
| 架构 | ARM64（默认） | macOS-latest 已是 ARM runner，无需额外指定 |

## 构建步骤

1. **Checkout** — 检出 tag 对应代码
2. **安装 OpenSSL** — `brew install openssl@3`（CMake find_package 需要）
3. **CMake 配置** — `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
4. **编译** — `cmake --build build`
5. **产物打包** — 创建 `web_server-macos-universal.tar.gz`（用户指定的命名，虽然只含 ARM 架构，命名保持通用）
6. **创建 Release** — 通过 `softprops/action-gh-release` action 创建 tag 对应的 Release
7. **上传产物** — 将压缩包附加到 Release 资产

## 产物

- **文件名**: `web_server-macos-universal.tar.gz`
- **内容**: 仅包含 `web_server` 可执行文件（strip 后）
- **使用方式**: 用户下载后 `tar xzf web_server-macos-universal.tar.gz && ./web_server --root .`

## 安全考量

- Workflow 使用 `GITHUB_TOKEN`（自动提供），无需手动配置凭据
- Release 创建和上传均通过官方 `softprops/action-gh-release` action 完成
- 不在 CI 中生成或泄露任何私钥/证书

## 不包含的范围

- **代码签名 / 公证**: 当前不涉及，后续可按需添加
- **Linux / Windows 构建**: 项目依赖 macOS 特有 API（kqueue, sendfile），暂不跨平台
- **自动测试**: 推送 tag 时只构建发布，不运行测试套件（可在 main 分支 push 时另设 CI 做验证）
