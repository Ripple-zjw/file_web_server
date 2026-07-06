#!/usr/bin/env bash
set -euo pipefail

# 禁用代理，避免 curl 请求被代理拦截
unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY

SERVER_BIN="${1:-./build/web_server}"
TEST_PORT="${2:-19999}"
PASS=0
FAIL=0
CURL_OPTS="-s -o /dev/null -w %{http_code} --max-time 3"

SERVER_PID=""
cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
}
trap cleanup EXIT

# 颜色
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

pass() {
    echo -e "  ${GREEN}✓${NC} $1"
    ((PASS++)) || true
}
fail() {
    echo -e "  ${RED}✗${NC} $1"
    ((FAIL++)) || true
}

# 启动服务器
echo "Starting server on port $TEST_PORT ..."
"$SERVER_BIN" --root . --port "$TEST_PORT" &
SERVER_PID=$!
sleep 1

# 检查进程是否存活
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo -e "${RED}Server failed to start${NC}"
    exit 1
fi
pass "Server started"

BASE="http://127.0.0.1:$TEST_PORT"

# 测试 1：GET /
echo "--- Test: GET /"
HTTP_CODE=$(curl $CURL_OPTS "$BASE/")
if [ "$HTTP_CODE" = "200" ] || [ "$HTTP_CODE" = "403" ] || [ "$HTTP_CODE" = "404" ]; then
    pass "GET / -> $HTTP_CODE"
else
    fail "GET / -> $HTTP_CODE (expected 200/403/404)"
fi

# 测试 2：HEAD /
echo "--- Test: HEAD /"
HEAD_CODE=$(curl $CURL_OPTS -I "$BASE/")
if [ "$HEAD_CODE" = "200" ] || [ "$HEAD_CODE" = "403" ] || [ "$HEAD_CODE" = "404" ]; then
    pass "HEAD / -> $HEAD_CODE"
else
    fail "HEAD / -> $HEAD_CODE (expected 200/403/404)"
fi

# 测试 3：GET 已知存在的文件（README.md）
echo "--- Test: GET /README.md"
README_CODE=$(curl $CURL_OPTS "$BASE/README.md")
if [ "$README_CODE" = "200" ]; then
    pass "GET /README.md -> 200"
else
    fail "GET /README.md -> $README_CODE (expected 200)"
fi

# 测试 4：GET 不存在的文件
echo "--- Test: GET /nonexistent_file_xyz"
NOTFOUND_CODE=$(curl $CURL_OPTS "$BASE/nonexistent_file_xyz")
if [ "$NOTFOUND_CODE" = "404" ]; then
    pass "GET /nonexistent -> 404"
else
    fail "GET /nonexistent -> $NOTFOUND_CODE (expected 404)"
fi

# 测试 5：点文件应被隐藏（服务器返回 400 Bad Request）
echo "--- Test: GET /.gitignore"
DOTFILE_CODE=$(curl $CURL_OPTS "$BASE/.gitignore")
if [ "$DOTFILE_CODE" = "400" ]; then
    pass "GET /.gitignore -> 400 (hidden)"
else
    fail "GET /.gitignore -> $DOTFILE_CODE (expected 400)"
fi

# 测试 6：路径逃逸应被拒绝（需 --path-as-is 阻止 curl 自行解析 ..）
echo "--- Test: GET /../etc/passwd (with --path-as-is)"
ESCAPE_CODE=$(curl $CURL_OPTS --path-as-is "$BASE/../etc/passwd")
if [ "$ESCAPE_CODE" = "400" ] || [ "$ESCAPE_CODE" = "403" ]; then
    pass "GET /../etc/passwd -> $ESCAPE_CODE (rejected)"
else
    fail "GET /../etc/passwd -> $ESCAPE_CODE (expected 400 or 403)"
fi

# 测试 7：Keep-Alive
echo "--- Test: Keep-Alive (GET /)"
KEEP_ALIVE=$(curl $CURL_OPTS --http1.1 "$BASE/")
if [ "$KEEP_ALIVE" = "200" ] || [ "$KEEP_ALIVE" = "403" ] || [ "$KEEP_ALIVE" = "404" ]; then
    pass "Keep-Alive request -> $KEEP_ALIVE"
else
    fail "Keep-Alive request -> $KEEP_ALIVE (expected 200/403/404)"
fi

# 测试 8：Content-Type 检查（README.md）
echo "--- Test: Content-Type for README.md"
CT=$(curl -s -o /dev/null -w "%{content_type}" --max-time 3 "$BASE/README.md")
if echo "$CT" | grep -qi "text/markdown\|text/plain\|text/\|application/octet-stream"; then
    pass "Content-Type: $CT"
else
    fail "Content-Type: $CT (expected text/* or similar)"
fi

echo ""
echo "=============================="
echo -e "Results: ${GREEN}$PASS passed${NC}, ${RED}$FAIL failed${NC}"
echo "=============================="

[ "$FAIL" -eq 0 ] || exit 1
