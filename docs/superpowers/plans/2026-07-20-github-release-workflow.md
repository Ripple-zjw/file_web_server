# GitHub Release 构建工作流 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 创建 GitHub Actions 工作流，在推送版本标签时自动在 macOS ARM64 runner 上编译 web_server Release 版本并上传到 GitHub Release。

**Architecture:** 单个 `.github/workflows/release.yml` 文件定义完整 CI 流水线，触发条件为 `v*` 标签，在 `macos-latest` runner 上执行：安装依赖 → CMake 构建 → 打包 → 发布到 Release。

**Tech Stack:** GitHub Actions, CMake, Homebrew, softprops/action-gh-release

## 全局约束

- 仅支持 macOS ARM64（Apple Silicon），不涉及交叉编译
- 产物命名：`web_server-macos-universal.tar.gz`（用户指定的命名）
- 构建类型必须是 `Release`（`-O3 -flto -DNDEBUG`）
- OpenSSL 通过 Homebrew 安装（`brew install openssl@3`）
- 只打包可执行文件，不包含证书、配置等

---

### 任务 1：创建 GitHub Actions Release 工作流文件

**文件:**
- Create: `.github/workflows/release.yml`

**说明：** 创建完整的 GitHub Actions 工作流，包括 checkout、安装依赖、CMake 构建、打包、创建 Release 并上传产物的全部步骤。

- [ ] **步骤 1：创建目录和文件**

创建 `.github/workflows/` 目录及 `release.yml` 文件。

- [ ] **步骤 2：编写工作流文件**

写入以下完整内容到 `.github/workflows/release.yml`：

```yaml
name: Release Build

on:
  push:
    tags:
      - 'v*'

jobs:
  build:
    name: Build Release on macOS ARM64
    runs-on: macos-latest

    steps:
      - name: Checkout code
        uses: actions/checkout@v4

      - name: Install OpenSSL
        run: brew install openssl@3

      - name: Configure CMake (Release)
        run: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

      - name: Build
        run: cmake --build build

      - name: Create archive
        run: |
          strip build/web_server
          tar czf web_server-macos-universal.tar.gz -C build web_server

      - name: Create Release and upload artifact
        uses: softprops/action-gh-release@v2
        with:
          files: web_server-macos-universal.tar.gz
          generate_release_notes: true
```

- [ ] **步骤 3：提交并推送到 GitHub**

```bash
git add .github/workflows/release.yml
git commit -m "ci: add GitHub Actions release workflow

在推送 v* 标签时自动在 macOS ARM64 runner 上构建 Release 版本，
打包为 web_server-macos-universal.tar.gz 并上传到 GitHub Release。

Co-Authored-By: Claude <noreply@anthropic.com>"
git push origin main
```

- [ ] **步骤 4：验证工作流**

推送到 GitHub 后，创建一个标签来触发构建验证：

```bash
git tag v0.1.0-test
git push origin v0.1.0-test
```

预期结果：
1. GitHub 仓库页面 → Actions 标签页 → 看到名为 "Release Build" 的 workflow 正在运行
2. 运行完成后，GitHub Releases 页面出现 `v0.1.0-test` 的 Release
3. Release 中包含 `web_server-macos-universal.tar.gz` 附件
4. 下载解压后，`./web_server --help` 正常运行

- [ ] **步骤 5：删除测试标签**

验证成功后删除测试标签（可选）：

```bash
git tag -d v0.1.0-test
git push origin --delete v0.1.0-test
```

后续正式发版只需要：
```bash
# 假设当前在 main 分支，代码已准备好
git tag v1.0.0
git push origin v1.0.0
```

GitHub Actions 会自动构建并创建 Release。
