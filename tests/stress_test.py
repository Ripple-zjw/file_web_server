#!/usr/bin/env python3
"""
Web Server 压力测试与稳定性测试

测试涵盖：
  1. 基础功能 — GET、HEAD、404、403
  2. Keep-Alive 连续多请求复用
  3. 大文件传输 — 5MB 文件
  4. Range 请求 — 单区间
  5. If-Modified-Since — 304/200
  6. 连接抖动 — 快速建连/断开
  7. 并发压力 — 线程池递增并发
  8. Keep-Alive 超时回收 — 验证链表正确清理
  9. 长时间持续负载 — 线程池大并发跑 15 秒

所有测试完成后验证服务器进程仍存活。
"""

import http.client
import os
import platform
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

# ── 配置 ──────────────────────────────────────────────────────────────
SERVER_BIN = os.path.join(os.path.dirname(__file__) or ".", "..", "build", "web_server")
SERVER_BIN = os.path.abspath(SERVER_BIN)

TEST_ROOT = "/tmp/stress_test_root"
TEST_PORT = 18999
BASE_URL = f"http://127.0.0.1:{TEST_PORT}"

TIMEOUT_LONG  = 75   # 大部分测试用
TIMEOUT_SHORT = 4    # 超时回收测试用

PASS = 0
FAIL = 0
server_proc = None


# ── 辅助函数 ─────────────────────────────────────────────────────────

def color(s, code):
    return f"\033[{code}m{s}\033[0m" if sys.stderr.isatty() else s

def green(s):  return color(s, "0;32")
def red(s):    return color(s, "0;31")
def yellow(s): return color(s, "0;33")
def cyan(s):   return color(s, "0;36")

def ok(msg):
    global PASS; PASS += 1
    print(f"  {green('✓')} {msg}")

def nok(msg):
    global FAIL; FAIL += 1
    print(f"  {red('✗')} {msg}")

def skip(msg):
    print(f"  {yellow('∼')} {msg}")

def section(title):
    print(f"\n{cyan('═══')} {title} {cyan('═' * (56 - len(title)))}")


def ensure_server_running():
    if server_proc is None:
        return False
    rc = server_proc.poll()
    if rc is not None:
        print(red(f"  ⚠ 服务器已退出，返回码 {rc}"))
        return False
    return True


def raw_http_get(path, extra_headers=None, timeout=5):
    """
    通过原始 socket 发送 HTTP/1.1 GET 请求（Connection: close）。
    返回 (status_code, body_bytes, headers_dict)。
    Python 的 urllib 会自己吞掉 Range/If-Modified-Since 等头部，
    所以用原始 socket 绕过。
    """
    hdrs = extra_headers or []
    req = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{TEST_PORT}\r\n"
    )
    for k, v in hdrs:
        req += f"{k}: {v}\r\n"
    req += "Connection: close\r\n\r\n"

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        sock.connect(("127.0.0.1", TEST_PORT))
        sock.sendall(req.encode())

        data = b""
        while True:
            try:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                data += chunk
            except socket.timeout:
                break
        sock.close()
    except Exception as e:
        return None, str(e).encode(), {}

    # 解析响应
    header_end = data.find(b"\r\n\r\n")
    if header_end == -1:
        return None, data, {}

    raw_headers = data[:header_end].decode("utf-8", errors="replace")
    body = data[header_end + 4:]

    # 提取状态码
    first_line = raw_headers.split("\r\n")[0]
    parts = first_line.split(" ")
    code = int(parts[1]) if len(parts) >= 2 else None

    # 提取响应头
    headers = {}
    for line in raw_headers.split("\r\n")[1:]:
        if ": " in line:
            k, v = line.split(": ", 1)
            headers[k.lower()] = v

    return code, body, headers


# ── 服务器管理 ───────────────────────────────────────────────────────

def start_server(timeout):
    global server_proc
    cmd = [SERVER_BIN, "--root", TEST_ROOT, "--port", str(TEST_PORT),
           "--timeout", str(timeout)]
    print(f"  启动: {' '.join(cmd)}")
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(20):
        if proc.poll() is not None:
            print(red(f"  服务器启动失败，返回码 {proc.returncode}"))
            return None
        try:
            urllib.request.urlopen(f"{BASE_URL}/", timeout=1)
            server_proc = proc
            return proc
        except Exception:
            time.sleep(0.2)
    print(red("  服务器启动超时"))
    proc.kill()
    return None


def stop_server():
    global server_proc
    if server_proc is None:
        return
    proc, server_proc = server_proc, None
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def restart_server(timeout, retries=3):
    """重启服务器，自动重试端口仍被占用的情况"""
    for attempt in range(retries):
        stop_server()
        time.sleep(1 + attempt)  # 递增等待
        proc = start_server(timeout)
        if proc is not None:
            return proc
        print(f"  重试 {attempt + 1}/{retries}...")
    return None


# ── 并发请求辅助 ─────────────────────────────────────────────────────

def concurrent_keepalive(num_requests, concurrency, path="/small.txt",
                         timeout=10):
    """Keep-Alive 连接池并发 GET"""
    results = {"ok": 0, "fail": 0, "err": 0}
    lock = threading.Lock()

    def worker():
        try:
            conn = http.client.HTTPConnection("127.0.0.1", TEST_PORT, timeout=timeout)
            for _ in range((num_requests + concurrency - 1) // concurrency):
                try:
                    conn.request("GET", path)
                    resp = conn.getresponse()
                    resp.read()
                    with lock:
                        if resp.status == 200:
                            results["ok"] += 1
                        else:
                            results["fail"] += 1
                except Exception:
                    with lock:
                        results["err"] += 1
            conn.close()
        except Exception:
            with lock:
                results["err"] += (num_requests + concurrency - 1) // concurrency

    threads = [threading.Thread(target=worker) for _ in range(concurrency)]
    t0 = time.monotonic()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    elapsed = time.monotonic() - t0
    return results["ok"], results["fail"], results["err"], elapsed


def concurrent_short(num_requests, concurrency, path="/small.txt",
                     timeout=10):
    """短连接（HTTP/1.0）并发 GET"""
    results = {"ok": 0, "fail": 0, "err": 0}
    lock = threading.Lock()

    def worker():
        for _ in range((num_requests + concurrency - 1) // concurrency):
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(timeout)
                sock.connect(("127.0.0.1", TEST_PORT))
                sock.sendall(
                    f"GET {path} HTTP/1.0\r\n"
                    f"Host: 127.0.0.1\r\n\r\n".encode()
                )
                data = sock.recv(65536)
                sock.close()
                if data and b"200" in data[:32]:
                    with lock: results["ok"] += 1
                else:
                    with lock: results["fail"] += 1
            except Exception:
                with lock: results["err"] += 1

    threads = [threading.Thread(target=worker) for _ in range(concurrency)]
    t0 = time.monotonic()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    elapsed = time.monotonic() - t0
    return results["ok"], results["fail"], results["err"], elapsed


# ── 测试用例 ──────────────────────────────────────────────────────────

def test_basic():
    section("1. 基础功能")

    # 1a. 根目录
    code, body, _ = raw_http_get("/")
    if code and code in (200, 403):
        ok(f"GET / → {code} ({len(body)} bytes)")
    else:
        nok(f"GET / → {code} (预期 200 或 403)")

    # 1b. 存在的文件
    code, body, _ = raw_http_get("/index.html")
    if code == 200:
        ok(f"GET /index.html → 200 ({len(body)} bytes)")
    else:
        nok(f"GET /index.html → {code} (预期 200)")

    # 1c. 404
    code, _, _ = raw_http_get("/nonexistent_file_xyz")
    if code == 404:
        ok("GET /nonexistent → 404")
    else:
        nok(f"GET /nonexistent → {code} (预期 404)")

    # 1d. 点文件 → 400
    code, _, _ = raw_http_get("/.gitignore")
    if code == 400:
        ok("GET /.gitignore → 400 (点文件被禁止)")
    else:
        nok(f"GET /.gitignore → {code} (预期 400)")

    # 1e. 路径逃逸
    code, _, _ = raw_http_get("/../etc/passwd")
    if code in (400, 403):
        ok(f"GET /../etc/passwd → {code} (路径逃逸被拒绝)")
    else:
        nok(f"GET /../etc/passwd → {code} (预期 400 或 403)")

    # 1f. HEAD
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(3)
        sock.connect(("127.0.0.1", TEST_PORT))
        sock.sendall(b"HEAD /index.html HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n")
        data = sock.recv(4096)
        sock.close()
        head_code = int(data.split(b" ")[1])
        # HEAD 不应返回 body
        body_part = data.split(b"\r\n\r\n")[1] if b"\r\n\r\n" in data else b""
        if head_code == 200 and len(body_part) == 0:
            ok(f"HEAD /index.html → 200 (body 为空 ✓)")
        else:
            nok(f"HEAD /index.html → {head_code}, body_len={len(body_part)}")
    except Exception as e:
        nok(f"HEAD 测试异常: {e}")

    # 1g. Content-Type
    code, body, hdrs = raw_http_get("/index.html")
    ct = hdrs.get("content-type", "")
    if code == 200 and "text/html" in ct:
        ok(f"Content-Type: {ct}")
    else:
        nok(f"Content-Type: {ct} (预期 text/html)")
    ensure_server_running()


def test_keepalive_multi_request():
    section("2. Keep-Alive 多请求复用")

    import http.client
    conn = http.client.HTTPConnection("127.0.0.1", TEST_PORT, timeout=5)
    try:
        for i in range(10):
            conn.request("GET", "/index.html")
            resp = conn.getresponse()
            body = resp.read()
            if resp.status != 200:
                nok(f"请求 {i+1}: GET /index.html → {resp.status}")
                break
            if i == 0 and len(body) == 0:
                nok("请求 1: 响应体为空")
                break
        else:
            ok("10 个连续请求（同一连接）全部 200")
    except Exception as e:
        nok(f"Keep-Alive 测试异常: {e}")
    finally:
        conn.close()
    ensure_server_running()


def test_large_file():
    section("3. 大文件传输 (5MB)")

    code, body, hdrs = raw_http_get("/large.bin", timeout=30)
    if code == 200:
        cl = int(hdrs.get("content-length", 0))
        if len(body) == 5242880:
            ok("GET /large.bin → 200, 5MB 完全接收")
        elif cl > 0 and len(body) == cl:
            ok(f"GET /large.bin → 200, {len(body)} bytes (Content-Length={cl})")
        else:
            nok(f"GET /large.bin → 200, 但 size={len(body)}, CL={cl}")
    else:
        nok(f"GET /large.bin → {code} (预期 200)")
    ensure_server_running()


def test_range():
    section("4. Range 请求")

    # 4a. 单区间
    code, body, hdrs = raw_http_get("/small.txt",
                                     extra_headers=[("Range", "bytes=0-9")])
    if code == 206 and len(body) == 10:
        ok(f"Range: bytes=0-9 → 206, {len(body)} bytes")
    else:
        nok(f"Range: bytes=0-9 → {code}, size={len(body)} (预期 206/10)")

    # 4b. Content-Range 头部
    code, body, hdrs = raw_http_get("/small.txt",
                                     extra_headers=[("Range", "bytes=5-14")])
    cr = hdrs.get("content-range", "")
    if code == 206:
        ok(f"Content-Range: {cr}")
    else:
        nok(f"Range 5-14 → {code} (预期 206)")
    ensure_server_running()


def test_if_modified_since():
    section("5. If-Modified-Since 条件请求")

    # 获取文件的 Last-Modified
    _, _, hdrs = raw_http_get("/small.txt")
    lm = hdrs.get("last-modified", "")

    if not lm:
        _, _, hdrs = raw_http_get("/subdir/index.html")
        lm = hdrs.get("last-modified", "")

    if lm:
        # 5a. 相同时间 → 304
        code, body, _ = raw_http_get("/small.txt",
                                      extra_headers=[("If-Modified-Since", lm)])
        if code == 304:
            ok(f"If-Modified-Since ({lm}) → 304 (未修改)")
        else:
            nok(f"If-Modified-Since → {code} (预期 304)")

        # 5b. 过去时间 → 200
        code, body, _ = raw_http_get("/small.txt",
                                      extra_headers=[
                                          ("If-Modified-Since",
                                           "Thu, 01 Jan 1970 00:00:00 GMT")
                                      ])
        if code == 200:
            ok("If-Modified-Since (过去) → 200 (已修改)")
        else:
            nok(f"If-Modified-Since (过去) → {code} (预期 200)")
    else:
        skip("未找到 Last-Modified 头部，跳过条件请求测试")
    ensure_server_running()


def test_connection_churn():
    section("6. 连接抖动")

    success = 0
    for i in range(200):
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(3)
            sock.connect(("127.0.0.1", TEST_PORT))
            sock.sendall(
                b"GET /small.txt HTTP/1.1\r\n"
                b"Host: 127.0.0.1\r\n"
                b"Connection: close\r\n\r\n"
            )
            resp = sock.recv(4096)
            sock.close()
            if b"200" in resp[:16]:
                success += 1
        except Exception:
            pass

    if success >= 180:
        ok(f"200 次快速连接，{success} 次成功 (≥90%)")
    else:
        nok(f"200 次快速连接，仅 {success} 次成功 (预期 ≥180)")
    ensure_server_running()


def test_concurrent():
    section("7. 并发压力测试")

    sc_ka = [
        ("500 请求, 10 并发 (短连接)", 500, 10, False),
        ("500 请求, 50 并发 (短连接)", 500, 50, False),
        ("500 请求, 50 并发 (Keep-Alive)", 500, 50, True),
    ]

    for label, n, c, ka in sc_ka:
        if not ensure_server_running():
            nok("服务器已退出，中止并发测试")
            return
        if ka:
            ok_c, fail_c, err_c, elapsed = concurrent_keepalive(n, c)
        else:
            ok_c, fail_c, err_c, elapsed = concurrent_short(n, c)
        rps = ok_c / elapsed if elapsed > 0 else 0
        if ok_c + fail_c >= n * 0.9 and err_c == 0:
            ok(f"{label} — {ok_c} ok, {err_c} err, "
               f"{rps:.0f} req/s ({elapsed:.1f}s)")
        else:
            nok(f"{label} — {ok_c} ok, {fail_c} fail, "
                f"{err_c} err, {rps:.0f} req/s")

    # 大文件并发
    if ensure_server_running():
        ok_c, fail_c, err_c, elapsed = concurrent_keepalive(
            200, 20, path="/medium.bin")
        rps = ok_c / elapsed if elapsed > 0 else 0
        if ok_c >= 180 and err_c == 0:
            ok(f"100KB 文件并发 (200×20, KA) — {ok_c} ok, "
               f"{rps:.0f} req/s ({elapsed:.1f}s)")
        else:
            nok(f"100KB 文件并发 — {ok_c} ok, {err_c} err")

    ensure_server_running()


def test_keepalive_timeout():
    section("8. Keep-Alive 超时回收")

    proc = restart_server(TIMEOUT_SHORT)
    if proc is None:
        nok("无法启动服务器 (短超时)，跳过超时测试")
        return

    import http.client
    connections = []
    for i in range(5):
        try:
            conn = http.client.HTTPConnection("127.0.0.1", TEST_PORT, timeout=5)
            conn.request("GET", "/index.html")
            resp = conn.getresponse()
            resp.read()
            if resp.status == 200:
                connections.append(conn)
            else:
                conn.close()
        except Exception:
            pass

    if len(connections) < 4:
        nok(f"只建立了 {len(connections)}/5 个 keep-alive 连接")
        stop_server()
        return

    ok(f"建立了 {len(connections)} 个 keep-alive 连接")

    # 等待超时过期
    time.sleep(TIMEOUT_SHORT + 2)

    if not ensure_server_running():
        nok("超时等待后服务器已崩溃")
        for c in connections: c.close()
        stop_server()
        return

    # 尝试在旧连接上发请求
    reused_ok = 0
    for conn in connections:
        try:
            conn.request("GET", "/small.txt")
            resp = conn.getresponse()
            resp.read()
            reused_ok += 1
        except Exception:
            pass

    if reused_ok == 0:
        ok(f"所有 {len(connections)} 个旧连接已被服务端关闭")
    elif reused_ok <= len(connections) // 2:
        ok(f"{reused_ok}/{len(connections)} 个旧连接被服务端关闭")
    else:
        nok(f"{reused_ok}/{len(connections)} 个旧连接仍可用"
            "（可能存在超时未回收）")

    # 验证新连接正常工作
    code, _, _ = raw_http_get("/index.html")
    if code == 200:
        ok("新连接正常工作（服务器未崩溃）")
    else:
        nok(f"新连接返回 {code}")

    for c in connections:
        c.close()

    ensure_server_running()

    # 切回长超时
    restart_server(TIMEOUT_LONG)


def test_sustained_load():
    section("9. 长时间持续负载 (15s)")

    if not ensure_server_running():
        nok("服务器已退出")
        return

    import http.client
    paths = ["/index.html", "/small.txt", "/medium.bin", "/subdir/index.html"]
    results = {"ok": 0, "fail": 0, "err": 0}
    lock = threading.Lock()
    stop_flag = threading.Event()

    def worker():
        try:
            conn = http.client.HTTPConnection("127.0.0.1", TEST_PORT, timeout=5)
            while not stop_flag.is_set():
                try:
                    path = paths[id(threading.current_thread()) % len(paths)]
                    conn.request("GET", path)
                    resp = conn.getresponse()
                    resp.read()
                    with lock:
                        if resp.status == 200:
                            results["ok"] += 1
                        else:
                            results["fail"] += 1
                except (http.client.HTTPException, OSError):
                    # 连接断了，重建
                    with lock: results["err"] += 1
                    try: conn.close()
                    except: pass
                    try:
                        conn = http.client.HTTPConnection("127.0.0.1", TEST_PORT, timeout=5)
                    except:
                        break
            conn.close()
        except Exception:
            pass

    concurrency = 20
    threads = [threading.Thread(target=worker) for _ in range(concurrency)]
    t0 = time.monotonic()
    for t in threads:
        t.start()
    time.sleep(15)
    stop_flag.set()
    for t in threads:
        t.join()
    elapsed = time.monotonic() - t0

    total = results["ok"] + results["fail"] + results["err"]
    rps = total / elapsed if elapsed > 0 else 0

    if results["ok"] > 0:
        err_pct = 100 * results["err"] / total if total > 0 else 0
        ok(f"15 秒混合负载 — {results['ok']} ok, {results['fail']} fail, "
           f"{results['err']} err ({err_pct:.0f}% fail), "
           f"{rps:.0f} req/s total ({elapsed:.1f}s)")
    else:
        nok("15 秒混合负载 — 全部失败")

    ensure_server_running()


# ── 主流程 ──────────────────────────────────────────────────────────

def main():
    banner = r"""
   ╔══════════════════════════════════════════╗
   ║       Web Server 压力测试套件             ║
   ║       Stress & Stability Test Suite       ║
   ╚══════════════════════════════════════════╝
    """
    print(banner)
    print(f"  服务器:  {SERVER_BIN}")
    print(f"  文档根:  {TEST_ROOT}")
    print(f"  端口:    {TEST_PORT}")
    print(f"  系统:    {platform.system()} {platform.machine()}")
    print(f"  Python:  {platform.python_version()}")

    if not os.path.exists(SERVER_BIN):
        print(red(f"\n  错误: {SERVER_BIN} 不存在，请先编译"))
        sys.exit(1)

    print(f"\n{cyan('═══')} 启动服务器 {cyan('═' * 48)}")
    proc = start_server(TIMEOUT_LONG)
    if proc is None:
        print(red("  无法启动服务器，中止"))
        sys.exit(1)
    ok(f"服务器启动 (PID={proc.pid}, timeout={TIMEOUT_LONG}s)")

    test_basic()
    test_keepalive_multi_request()
    test_large_file()
    test_range()
    test_if_modified_since()
    test_connection_churn()
    test_concurrent()
    test_sustained_load()

    print()
    test_keepalive_timeout()

    # 最终检查
    section("最终检查")
    if ensure_server_running():
        ok("服务器在全部测试后仍存活 ✓")
    else:
        nok("服务器已崩溃 ✗")

    print(f"\n{'=' * 58}")
    total = PASS + FAIL
    color_fn = green if FAIL == 0 else red
    print(color_fn(f"  结果: {PASS} passed, {FAIL} failed / {total} total"))
    print(f"{'=' * 58}")

    stop_server()

    if FAIL > 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
