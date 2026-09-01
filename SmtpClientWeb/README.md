# SMTP 客户端网页终端（浏览器 ↔ SMTP 服务器演示）

一个纯文字交互的 **SMTP 客户端网页**，用来直观展示
**SMTP 客户端 ↔ 服务器** 的完整对话过程，适合课程答辩 / 效果演示。

## 架构

```
浏览器（网页终端）
   │  HTTP/JSON（浏览器 JS 不能直连 TCP）
   ▼
bridge（C++ 桥接服务器，8888 端口）
   │  TCP + SMTP 协议（EHLO / MAIL / RCPT / DATA / QUIT）
   ▼
MailServer（SMTP 服务器，127.0.0.1:2525）
```

## 快速开始

```bash
./start_demo.sh
# 然后浏览器打开 http://localhost:8888
```

或手动分两步：

```bash
# 1) 启动 SMTP 服务器（另一个终端）
cd MailServer
g++ -std=c++17 -O2 -pthread -o smtp_server main.cpp src/SmtpServer.cpp src/Server.cpp -Iinclude
./smtp_server

# 2) 启动网页桥接（本目录）
cd SmtpClientWeb
make
./bridge
```

## 使用方法

1. 打开页面会自动连接服务器，看到 `220` 问候语
2. 在输入框按顺序输入 SMTP 命令（也可以点上方快捷按钮）：
   - `EHLO localhost`
   - `MAIL FROM:<alice@example.com>`
   - `RCPT TO:<bob@example.com>`
   - `DATA`（进入 DATA 模式后输入多行正文，自动补 `.` 结束）
   - `QUIT`
3. 也可以点「▶ 自动演示」一键跑完整流程
4. 收到的邮件会保存在 `MailServer/mailbox/` 目录

> 蓝色 = 客户端命令，绿色 = 服务器响应，黄色 = 系统提示。

## HTTP 接口（bridge 提供）

| 接口 | 说明 |
|---|---|
| `GET /` | 网页 |
| `POST /api/connect` | 建立 SMTP 会话，返回 `220` 问候语；响应头 `X-Session` 为会话 id |
| `POST /api/command` | 发送一行命令（DATA 模式下发送多行正文），返回服务器完整响应 |
| `POST /api/disconnect` | 发送 `QUIT` 并关闭会话 |
