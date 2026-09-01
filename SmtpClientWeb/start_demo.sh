#!/usr/bin/env bash
# 一键启动 SMTP 演示环境：
#   1) 编译并启动 SMTP 服务器（127.0.0.1:2525）
#   2) 编译并启动浏览器桥接（http://localhost:8888）
set -euo pipefail
cd "$(dirname "$0")"

SRV_DIR="../MailServer"

echo "[1/3] 编译 SMTP 服务器 ..."
g++ -std=c++17 -O2 -pthread -o "$SRV_DIR/smtp_server" \
    "$SRV_DIR/main.cpp" "$SRV_DIR/src/SmtpServer.cpp" "$SRV_DIR/src/Server.cpp" -I"$SRV_DIR/include"

echo "[2/3] 编译浏览器桥接服务器 ..."
g++ -std=c++17 -O2 -Wall -Wextra -pthread -o bridge bridge.cpp

echo "[3/3] 启动服务 ..."
(cd "$SRV_DIR" && ./smtp_server) &   # 在 MailServer 目录内运行，邮件会存到 MailServer/mailbox/
SMTP_PID=$!
trap 'echo; echo "== 停止 SMTP 服务器 ..."; kill "$SMTP_PID" 2>/dev/null || true' EXIT INT TERM
sleep 0.5

echo "=============================================="
echo "  请在浏览器打开:  http://localhost:8888"
echo "  按 Ctrl+C 退出"
echo "=============================================="
./bridge
