# MailForge —— 邮件服务器（C/C++ 原生实现 SMTP + POP3）

> 计算机与网络课程设计 —— 邮件协议实现及应用系统研制
> 选题序号 **18** ｜ 难度系数 **1.0**

**MailForge** 是一个用 **C/C++ 从零编写**的邮件服务器，**不依赖任何第三方库**：
基于原生 **Socket 编程**实现了 **SMTP（RFC 5321）发送**与 **POP3（RFC 1939）收取**两大标准协议，
并支持**多用户邮箱隔离**（每个用户一个独立收件目录，邮件以标准 `.eml` 格式落盘，服务器重启不丢失）。

> 本文档依据仓库里**当前实际存在的代码**编写，逐文件、逐函数、逐结构体地说明
> 「输入参数 / 返回值 / 作用 / 注意事项」，方便复习、答辩与后续扩展。

---

## 1. 当前项目能做什么

| 能力 | 说明 | 状态 |
|---|---|---|
| SMTP 发信 | 完整交互：`HELO/EHLO → MAIL FROM → RCPT TO → DATA → QUIT` | ✅ 已实现 |
| 邮件落盘 | 按**收件人**自动投递到 `./mailbox/<用户名>/`，存为 `.eml` | ✅ 已实现 |
| POP3 收信 | 完整交互：`USER/PASS` 认证 + `STAT/LIST/RETR/DELE/RSET/NOOP/QUIT` | ✅ 已实现 |
| 多用户隔离 | 每个账号独立的收件目录，互不可见 | ✅ 已实现 |
| 账号管理 | 文本账号表 `users.txt`（`用户名:密码`，一行一个） | ✅ 已实现 |
| 并发连接 | 每个客户端一个线程（`std::thread`） | ✅ 已实现 |
| 邮件加密 / Web 前端 | 尚未实现，属于后续里程碑 | ⏳ 规划中 |

## 2. 端口约定

| 服务 | 标准端口 | 本项目使用 | 原因 |
|---|---|---|---|
| SMTP（发信） | 25 | **2525** | 25 需要 root 权限且常被防火墙拦截；2525 是高端口，普通用户可直接监听 |
| POP3（收信） | 110 | **1110** | 同上，110 也需要 root 权限 |

> 用 telnet / 邮件客户端 / Python 测试时，端口填 **2525 / 1110**。

## 3. 架构总览

```
                    ┌───────────────────────────────────────────────┐
   SMTP 客户端 ────► │  MailServer（本仓库 MailServer/ 目录）            │
   (Outlook/python)  │                                               │
                    │   main.cpp                                     │
                    │     ├── std::thread ──► Pop3Server::start()    │
                    │     └── 主线程      ──► SmtpServer::start()    │
                    │                                               │
                    │   ┌────────────────────────────────────────┐   │
                    │   │ Server（基类：socket→bind→listen→accept） │   │
                    │   │     每来一个连接开线程调 handleClient()     │   │
                    │   └───────────────▲──────────────▲──────────┘   │
                    │                   │              │              │
                    │         SmtpServer（2525）  Pop3Server（1110）   │
                    │         processCommand     processCommand      │
                    └───────────────────┼──────────────┼──────────────┘
                                        ▼              ▼
                                    ./mailbox/<用户名>/xxx.eml
                                       （SMTP 写入 / POP3 读取）
```

设计要点：
- **网络层与协议层分离**：`Server` 基类把 socket / bind / listen / accept / 多线程全部封装好；
  子类只需重写纯虚函数 `handleClient()`，专心写协议。
- **同一个存储**：SMTP 投递进 `./mailbox/<用户名>/`，POP3 登录后读的也是同一个目录，
  收发天然打通，这就是"多邮箱账户隔离"。

## 4. 目录结构（按实际代码）

```
MailForge/
├── MailServer/                    # ★ 当前主线：邮件服务器
│   ├── main.cpp                   # 程序入口：同时启动 SMTP(2525) + POP3(1110)
│   ├── include/
│   │   ├── Server.h               # 网络基类 Server（纯虚函数 handleClient）
│   │   ├── SmtpServer.h           # SMTP 服务器类 + SmtpMail 结构体
│   │   └── Pop3Server.h           # POP3 服务器类 + Pop3Mail / Pop3State 结构体
│   ├── src/
│   │   ├── Server.cpp             # Server 基类实现
│   │   ├── SmtpServer.cpp         # SMTP 协议实现（状态机 + 落盘）
│   │   └── Pop3Server.cpp         # POP3 协议实现（状态机 + 读目录 + 删除）
│   ├── users.txt                  # POP3 账号表（默认 bob/alice，密码 123456）
│   ├── mailbox/                   # 邮件存储根目录（运行时自动创建）
│   │   └── bob/                   # bob 的收件目录（内含 2 封测试邮件 *.eml）
│   ├── mail_server                # 编译产物（当前主线可执行文件）
│   └── smtp_server                # 旧编译产物（早期单独编译的 SMTP 程序）
├── 连接测试/                      # 里程碑 1 的练手代码（HTTP 连通性测试服务器）
│   ├── server.cpp  utils.h  utils.cpp
│   └── server                     # 编译产物
├── SmtpClientWeb/                 # 早期"浏览器发信"联调残留，仅剩 bridge 二进制（无源码）
│   └── bridge
├── requirements.md                # 课程需求文档
├── setup_portproxy.bat            # Windows 端口转发一键配置脚本（局域网/ZeroTier 用）
├── LICENSE  .gitignore  .vscode/
└── README.md                      # 本文档
```

## 5. 快速开始

### 5.1 编译

```bash
cd /home/phantasma/MailForge/MailServer
g++ -std=c++17 -pthread -o mail_server main.cpp src/Server.cpp src/SmtpServer.cpp src/Pop3Server.cpp -I include
```

### 5.2 运行

```bash
./mail_server
```

看到以下输出即成功（两个端口都在监听）：

```
[POP3] 已从 ./users.txt 加载 2 个账号
==============================================
 MailForge MailServer 启动
   SMTP 服务器：端口 2525（发邮件用）
   POP3 服务器：端口 1110（收邮件用）
==============================================
[服务器] 已启动，监听端口 2525
[服务器] 已启动，监听端口 1110
```

### 5.3 默认账号

| 用户名 | 密码 | 收件目录 |
|---|---|---|
| `bob` | `123456` | `./mailbox/bob/` |
| `alice` | `123456` | `./mailbox/alice/` |

修改 / 新增账号：编辑 `users.txt`，一行一个 `用户名:密码`，`#` 开头是注释。

### 5.4 手动体验收信（telnet）

```text
telnet 127.0.0.1 1110
+OK MailForge POP3 server ready
USER bob              → +OK User bob known
PASS 123456           → +OK mailbox ready, 2 message(s), ... bytes
STAT                  → +OK 2 ...
LIST                  → 列出 2 封邮件
RETR 1                → 下载第 1 封（多行，以 . 结束）
DELE 1                → 标记删除（QUIT 时才真正删文件）
RSET                  → 撤销删除标记
QUIT                  → +OK ...（此时被 DELE 的邮件才真正从磁盘删除）
```
---

## 6. 协议速览（先看懂协议，再读代码）

### 6.1 SMTP（简单邮件传输协议，RFC 5321）—— 用来"发"

基于 TCP 的文本协议，**一问一答**：客户端发一行命令，服务器回一行「状态码 + 说明」。

**一次标准会话流程：**

```text
客户端                          服务器(2525)
  │  建立 TCP 连接                  │
  │◄────────────────────────── 220 MyMailServer ESMTP ready
  │  EHLO 我的主机名                │
  │───────────────────────────►    │
  │◄────────────────────────── 250 Hello 我的主机名,nice to meet you
  │  MAIL FROM:<alice@example.com>  │
  │───────────────────────────►    │
  │◄────────────────────────── 250 OK
  │  RCPT TO:<bob@example.com>      │
  │───────────────────────────►    │
  │◄────────────────────────── 250 OK
  │  DATA                           │
  │───────────────────────────►    │
  │◄────────────────────────── 354 End data with <CR><LF>.<CR><LF>
  │  邮件原文（头部+空行+正文，一行行发）│
  │  .   ← 单独一个点表示内容结束      │
  │───────────────────────────►    │
  │◄────────────────────────── 250 Message accepted for delivery
  │  QUIT                           │
  │───────────────────────────►    │
  │◄────────────────────────── 221 Bye
```

**本服务器支持的 SMTP 命令与响应：**

| 命令 | 含义 | 正常响应 | 出错响应 |
|---|---|---|---|
| `HELO/EHLO <主机名>` | 打招呼（大小写不敏感） | `250 Hello ...` | — |
| `MAIL FROM:<地址>` | 声明发件人 | `250 OK` | `501 Syntax error in MAIL FROM` |
| `RCPT TO:<地址>` | 声明收件人 | `250 OK` | `501 Syntax error in RCPT TO` |
| `DATA` | 开始传正文 | `354 End data with <CR><LF>.<CR><LF>` | `503 ...need MAIL and RCPT first`（没先声明收发件人） |
| `.`（DATA 中单独一行） | 正文结束 | `250 Message accepted for delivery` | — |
| `QUIT` | 退出 | `221 Bye` | — |
| 其他 | — | — | `500 Unrecognized command` |

**点填充（dot-stuffing）**：邮件正文里任何以 `.` 开头的行，客户端必须先写成 `..`，
否则服务器会误以为正文结束了；服务器收到 `..` 开头时会把多余的 `.` 去掉再存盘。

### 6.2 POP3（邮局协议第三版，RFC 1939）—— 用来"收"

同样是 TCP 文本协议，但响应用 **`+OK` / `-ERR`** 开头，多行内容以单独一行的 `.` 结束。

**三个阶段（状态机）：**

```text
连接建立 ─► AUTHORIZATION 认证态 ──PASS 通过──► TRANSACTION 事务态 ──QUIT──► UPDATE 更新态
          (只允许 USER/PASS/QUIT)              (STAT/LIST/RETR/DELE/...)     (真正删除被 DELE 的邮件)
```

**本服务器支持的 POP3 命令与响应：**

| 命令 | 可用状态 | 含义 | 正常响应 | 出错响应 |
|---|---|---|---|---|
| `USER <用户名>` | 认证态 | 提交用户名 | `+OK User bob known` | `-ERR User xxx unknown` |
| `PASS <密码>` | 认证态 | 提交密码 | `+OK mailbox ready, N message(s), ...` | `-ERR invalid password` |
| `STAT` | 事务态 | 邮件数+总字节 | `+OK N bytes` | `-ERR not authenticated...` |
| `LIST` | 事务态 | 列邮件（多行） | `+OK N messages` + `编号 大小`… + `.` | 同上 |
| `LIST <编号>` | 事务态 | 查单封大小 | `+OK 编号 大小` | `-ERR no such message` |
| `RETR <编号>` | 事务态 | 下载一封（多行） | `+OK 大小 octets` + 内容 + `.` | `-ERR no such message` |
| `DELE <编号>` | 事务态 | 标记删除（不真删） | `+OK message N deleted` | `-ERR no such message (or already deleted)` |
| `RSET` | 事务态 | 撤销所有 DELE 标记 | `+OK all delete marks cleared` | — |
| `NOOP` | 事务态 | 保活（无操作） | `+OK` | — |
| `QUIT` | 任意 | 退出并真正删信 | `+OK MailForge POP3 server signing off` | — |

**POP3 的三个关键设计：**
1. **DELE 只是打标记**，只有 `QUIT` 才真正删文件；客户端中途断线邮件不会丢。
2. 消息编号在本会话内固定（`1..N`），删除某封后**其余编号不重排**（`LIST`/`STAT` 不再统计已删的）。
3. 传输邮件内容时，正文中以 `.` 开头的行要发成 `..`（点填充），防止和结束符 `.` 冲突。

### 6.3 邮件存储格式

- 存储根目录：`./mailbox/`（程序启动时自动创建）。
- 每个用户一个子目录：`./mailbox/<用户名>/`，用户名取收件人邮箱 `@` 前部分并转小写
  （`bob@example.com` → `./mailbox/bob/`）。
- 文件名：`<Unix时间戳>_<随机数>.eml`，如 `1788314490_846930886.eml`，
  加随机数是为了避免多线程并发投递时文件名冲突。
- 文件内容是**标准 RFC 5322 邮件**（头部区 + 空行 + 正文），示例：

```eml
Date: Wed Sep  2 10:00:52 2026
From: alice@example.com
To: bob@example.com
Subject: (no subject)

Subject: Hello from MailForge

这是一封测试邮件！
```

> 头部区：若客户端在 DATA 里写了 `From/To/Subject/Date` 就原样保留；
> 没写则服务器用信封信息（MAIL FROM / RCPT TO）与当前时间**补全**，保证 `.eml` 能被正常解析显示。

---

## 7. 代码详解 —— 网络基类（`MailServer/include/Server.h` + `src/Server.cpp`）

> 作用：把「建 socket → 设端口复用 → bind → listen → accept → 每连接一线程」这套
> **所有网络服务器都一样的流程**封装好。SMTP / POP3 都继承它，只需各自重写 `handleClient()`。
> 源码里把这段戏称为"模板方法模式"。

### 7.1 `class Server` 类总览

| 类型 | 名称 | 说明 |
|---|---|---|
| 私有成员 | `int server_fd` | 监听 socket 的文件描述符（-1 表示未创建/已关闭） |
| 私有成员 | `int port` | 要监听的端口号（构造时传入并保存） |
| 私有成员 | `std::atomic<bool> is_running` | 运行标志，控制 accept 循环是否继续（原子类型，多线程安全） |
| 私有 | `Server(const Server&)` 与 `operator=` | 已删除（`= delete`），禁止拷贝服务器对象 |
| 公开 | `Server(int port)` | 构造函数 |
| 公开 | `virtual ~Server()` | 虚析构函数 |
| 公开 | `bool start()` | 启动服务器（阻塞，见 7.3） |
| 公开 | `void stop()` | 请求停止服务器（见 7.4） |
| 保护 | `virtual void handleClient(int) = 0` | **纯虚函数**，子类必须实现 |

### 7.2 `Server::Server(int port)` —— 构造函数

| 项目 | 内容 |
|---|---|
| 输入参数 | `port`（int）：本服务器要监听的端口号 |
| 返回值 | 无 |
| 行为 | 用初始化列表把 `server_fd` 置 `-1`、保存 `port`、`is_running` 置 `false`。**只保存参数，不创建 socket**（真正的 socket 在 `start()` 里创建） |

### 7.3 `Server::~Server()` —— 析构函数

| 项目 | 内容 |
|---|---|
| 输入参数 | 无 |
| 返回值 | 无 |
| 行为 | 调用 `stop()`，确保服务器退出时关闭监听 socket、不遗留资源 |

### 7.4 `bool Server::start()` —— 启动服务器（核心流程）

| 项目 | 内容 |
|---|---|
| 输入参数 | 无（端口用构造函数保存的成员 `port`） |
| 返回值 | `bool`：`true` 表示流程走完正常返回；`false` 表示**中途失败**（已打印 `perror` 错误原因） |
| 阻塞 | **是**。成功后进入 accept 死循环，只有 `stop()` 被调用（或进程被杀）才会返回 |

按顺序执行：

1. `socket(AF_INET, SOCK_STREAM, 0)` —— 创建 IPv4 + TCP 流式 socket，失败返回 `false`。
2. `setsockopt(SO_REUSEADDR)` —— 设置**端口复用**，程序崩溃后能立刻重新绑定同一端口。
3. `bind()` —— 把 `0.0.0.0:port`（所有网卡）绑定到 `server_fd`。
4. `listen(server_fd, 5)` —— 开始监听，内核等待队列长度 5。
5. `is_running = true`，打印 `[服务器] 已启动，监听端口 XXXX`。
6. **accept 主循环**（`while(is_running)`）：
   - `accept()` 阻塞等待新客户端；返回 `client_fd`（负责和该客户端收发数据）；
   - 打印客户端 IP 和端口；
   - `std::thread worker([this, client_fd]{ handleClient(client_fd); close(client_fd); })`，
     `worker.detach()` —— **每个客户端开一条线程**处理协议，互不阻塞。
7. 退出循环后 `close(server_fd)`，打印 `[服务器] 已停止`，返回 `true`。

### 7.5 `void Server::stop()` —— 请求停止

| 项目 | 内容 |
|---|---|
| 输入参数 | 无 |
| 返回值 | 无 |
| 行为 | 把 `is_running` 置 `false`，并 `close(server_fd)`（close 后阻塞在 `accept` 的调用会立刻返回 -1，循环从而退出）。再把 `server_fd` 置 `-1` 防止重复 close |

### 7.6 `virtual void handleClient(int client_fd) = 0` —— 纯虚函数

| 项目 | 内容 |
|---|---|
| 输入参数 | `client_fd`（int）：当前客户端连接的 socket 句柄 |
| 返回值 | 无（`void`） |
| 说明 | 这是每个具体服务器的**协议入口**。基类不实现；`SmtpServer` 和 `Pop3Server` 各自重写（详见下文第 8、9 章）。它在线程里执行，返回即代表该客户端会话结束 |

> ⚠️ 历史 Bug 记录：`start()` 早期版本在 `socket()` 成功后多写了一个 `else { return true; }`，
> 导致下面 bind/listen/accept 全部成为**死代码**、服务器"秒退"；已修复（见第 13 章）。

---

## 8. 代码详解 —— SMTP 服务器（`MailServer/include/SmtpServer.h` + `src/SmtpServer.cpp`）

### 8.1 `struct SmtpMail` —— "一封邮件"的会话状态结构体

SMTP 发信是**分步**的（MAIL → RCPT → DATA），服务器需要把几步的信息**跨命令拼在一起**，
所以用这个结构体充当会话状态，跟随整个会话传递。

| 字段 | 类型 | 含义 | 何时被填充 |
|---|---|---|---|
| `from` | `std::string` | 信封**发件人**地址 | `MAIL FROM:<...>` 命令时 |
| `to` | `std::string` | 信封**收件人**地址 | `RCPT TO:<...>` 命令时 |
| `body` | `std::string` | DATA 阶段逐行收到的**完整邮件原文**（头部+空行+正文） | DATA 模式每收到一行就追加 |
| `headers` | `std::string` | 头部区原文（第一个空行之前，原样保留） | DATA 结束时由 `parseMailData()` 解析 |
| `text` | `std::string` | 正文（第一个空行之后） | 同上 |
| `headerFrom` | `std::string` | 头部里 `From:` 字段的值（客户端没写则为空） | 同上 |
| `headerTo` | `std::string` | 头部里 `To:` 字段的值 | 同上 |
| `subject` | `std::string` | 头部里 `Subject:` 字段的值 | 同上 |
| `headerDate` | `std::string` | 头部里 `Date:` 字段的值 | 同上 |

> 信封（envelope）≠ 头部（header）：`MAIL FROM`/`RCPT TO` 是传输时的"信封"，
> DATA 里的 `From:`/`To:` 是显示用的"头部"，两者可能不同，代码分别保存。

### 8.2 `class SmtpServer` 类总览

继承自 `Server`。公开接口只有构造函数；协议逻辑全部在私有/保护成员里。

| 类型 | 成员 | 说明（详见下文） |
|---|---|---|
| 公开 | `SmtpServer(int port)` | 构造函数：创建 `./mailbox` 目录 |
| 公开 | `~SmtpServer()` | 默认析构（没有额外资源要释放） |
| 保护 | `void handleClient(int)` | 重写：处理一个 SMTP 客户端会话 |
| 私有 | `bool sendAll(int, const char*, size_t)` | 底层"保证发完"的封装 |
| 私有 | `void sendResponse(int, const std::string&)` | 发一行 SMTP 响应（自动补 `\r\n`） |
| 私有 | `bool processCommand(int, const std::string&, SmtpMail&, bool&)` | **状态机核心**：解析并应答一条命令 |
| 私有 | `void parseMailData(SmtpMail&)` | 把收齐的正文拆成 头部/正文/各标准头 |
| 私有 | `std::string getHeaderValue(const std::string&, const std::string&)` | 在头部原文里按名字取值（大小写不敏感） |
| 私有 | `void saveMail(const SmtpMail&)` | 把邮件按收件人写入 `./mailbox/<用户名>/` |

### 8.3 `SmtpServer::SmtpServer(int port)` —— 构造函数

| 项目 | 内容 |
|---|---|
| 输入参数 | `port`（int）：SMTP 监听端口 |
| 返回值 | 无 |
| 行为 | ① 把 `port` 交给基类 `Server`；② `mkdir("./mailbox", 0755)` 创建邮件根目录（已存在则忽略错误） |

### 8.4 `bool SmtpServer::sendAll(int fd, const char* data, size_t len)` —— 保证发完

| 项目 | 内容 |
|---|---|
| 输入参数 | `fd`：客户端 socket；`data`：要发送的数据首地址；`len`：要发送的字节数 |
| 返回值 | `bool`：`true` = 全部发送完毕；`false` = 发送失败（连接已断等） |
| 原理 | 单次 `send()` 不一定一次发完、还可能被信号打断（返回 `EINTR`）。循环 `send`，每次从上次未发完的位置接着发，直到全部发出。`send` 返回 0 表示对端关闭，视为失败 |

### 8.5 `void SmtpServer::sendResponse(int fd, const std::string& response)` —— 发一行响应

| 项目 | 内容 |
|---|---|
| 输入参数 | `fd`：客户端 socket；`response`：响应文本（**不含换行**），如 `"250 OK"` |
| 返回值 | 无 |
| 行为 | 给响应补上 SMTP 协议规定的 `\r\n` 后调用 `sendAll` 发出；失败时打印日志 |
| 为什么不自己补 `\n` | RFC 规定 SMTP 所有行必须以 **CRLF（`\r\n`）** 结尾，严格客户端缺了 `\r` 会解析失败 |

### 8.6 `bool SmtpServer::processCommand(int fd, const std::string& line, SmtpMail& mail, bool& dataMode)` —— 命令状态机（核心）

| 项目 | 内容 |
|---|---|
| 输入参数 | `fd`：客户端 socket；`line`：客户端发来的一行内容（已去掉 `\r\n`）；`mail`：本会话拼装的邮件（**引用**，跨命令共享/修改）；`dataMode`：是否处于 DATA 模式（**引用**，函数内会改） |
| 返回值 | `bool`：`true` = 继续会话；`false` = 会话结束（收到 `QUIT`） |

**处理逻辑分两大段：**

**① DATA 模式中（`dataMode == true`）——这一行不是命令，是正文的一行：**
- 若 `line == "."`（单独一个点）：正文结束 → `dataMode=false` → `parseMailData()` 拆头 → 回 `250 Message accepted for delivery` → `saveMail()` 落盘 → 重置 `mail`（准备收下一封）；
- 否则：先做**点填充还原**（`..` 开头的行去掉一个点，变回 `.`），再把该行加 `\r\n` 追加进 `mail.body`。

**② 普通模式——解析命令（命令转大写后逐个匹配）：**

| 命令 | 处理细节 | 响应 |
|---|---|---|
| `HELO` / `EHLO` | 参数为空则问候语用 `unknown`，否则用参数 | `250 Hello <主机名>,nice to meet you` |
| `MAIL FROM:` | 去掉 `FROM:` 前缀、前导空格、首尾 `<>` 后得到地址；空地址算语法错误 | 成功 `250 OK`；失败 `501 Syntax error in MAIL FROM` |
| `RCPT TO:` | 同上解析收件人，存入 `mail.to` | 成功 `250 OK`；失败 `501 Syntax error in RCPT TO` |
| `DATA` | 若发件人/收件人都还没声明（`mail.from/to` 为空）则拒绝；否则进入 DATA 模式 | 成功 `354 End data with <CR><LF>.<CR><LF>`；失败 `503 ...need MAIL and RCPT first` |
| `QUIT` | 回复再见 | `221 Bye`，并返回 `false` 让会话结束 |
| 空行 | 直接忽略（什么也不回） | — |
| 其它未知命令 | 回通用错误 | `500 Unrecognized command` |

> ⚠️ 历史 Bug 记录：早期版本把"点填充还原"放错到普通模式分支，且后面所有正常命令解析
> 都成了**不可达代码**（服务器只打招呼、不应答任何命令）；另在 MAIL 分支后有一个**悬空的
> `return true`**，使 RCPT/DATA/QUIT 全部失效。均已修复（见第 13 章）。

### 8.7 `void SmtpServer::parseMailData(SmtpMail& mail)` —— 解析 DATA 原文

| 项目 | 内容 |
|---|---|
| 输入参数 | `mail`（引用）：其 `body` 字段必须已收齐完整邮件原文；函数会**修改**它的 `headers/text/headerFrom/headerTo/subject/headerDate` |
| 返回值 | 无 |
| 行为 | ① 跳过开头所有空行；② 在剩余内容里找 `\r\n\r\n`（头部与正文的分隔空行）：之前的是 `headers`，之后的是 `text`；③ 用 `getHeaderValue()` 分别提取 `From / To / Subject / Date` 存进对应字段 |

### 8.8 `std::string SmtpServer::getHeaderValue(const std::string& headers, const std::string& name)` —— 取某个头的值

| 项目 | 内容 |
|---|---|
| 输入参数 | `headers`：头部区原文（多行）；`name`：要找的字段名，如 `"Subject"` |
| 返回值 | `std::string`：该字段的值（已去掉前导空格/Tab，**去掉**行尾换行）；找不到返回空串 `""` |
| 规则 | ① 字段名**大小写不敏感**（RFC 5322）；② 支持**续行**（下一行以空格/Tab 开头时拼接到当前值后）；③ 不解析头部里的折叠异常情况，用于本项目够用 |

### 8.9 `void SmtpServer::handleClient(int client_fd)` —— 一个 SMTP 会话的完整生命周期

| 项目 | 内容 |
|---|---|
| 输入参数 | `client_fd`：客户端 socket（由基类线程传入） |
| 返回值 | 无 |
| 行为 | ① 先发问候 `220 MyMailServer ESMTP ready`；② 创建本会话的 `mail` 与 `dataMode`；③ **行缓冲循环**：`recv` 读 4KB 进 `buf`，按 `\n` 拆成一行行，去掉行尾 `\r`，交给 `processCommand()`；`buf` 里拆剩下的"半行"留在缓冲区等下次 `recv` 拼齐；④ `recv` 返回 0（对端断开）或 `processCommand` 返回 `false`（QUIT）时关闭 socket 结束线程 |

> 为什么要"行缓冲"：TCP 是**流式协议**，一次 `recv` 可能包含多条命令，也可能一条命令被拆成多次 `recv`。不按 `\n` 拆行就会把多条命令拼在一起、或把命令拦腰截断，导致响应错乱。

### 8.10 `void SmtpServer::saveMail(const SmtpMail& mail)` —— 把邮件写入用户收件目录

| 项目 | 内容 |
|---|---|
| 输入参数 | `mail`：一封已经解析完成的邮件（`from/to` 必须有值） |
| 返回值 | 无 |
| 行为 | ① 由 `mail.to`（信封收件人）取出 `@` 前部分 → 去首尾空白 → 转小写，得到用户名（解析不出则兜底存根目录并告警）；② `mkdir` 确保 `./mailbox/<用户名>/` 存在；③ 生成文件名 `<秒级时间戳>_<rand()>.eml`；④ 组装 RFC 5322 头部：`Date/From/To/Subject` 客户端 DATA 里**没写**时用信封信息+当前时间**补全**（Date 用 `ctime`，From 用 `mail.from`，To 用 `mail.to`，Subject 用占位 `(no subject)`），**写了**就保留客户端原文 `mail.headers`；⑤ 确保头部最后以 `\r\n` 结尾后，按「头部 + 空行 + 正文」写入文件；⑥ 关闭文件并打印保存路径与主题日志 |

---

## 9. 代码详解 —— POP3 服务器（`MailServer/include/Pop3Server.h` + `src/Pop3Server.cpp`）

> POP3 负责"收信"。代码结构与 SMTP 完全对称：同样继承 `Server`，
> 同样有 `sendAll/sendResponse/processCommand/handleClient`，区别只在协议命令与存储方向
> （SMTP **写**进 `./mailbox/<用户>/`，POP3 从那里**读**，并在 QUIT 时按标记删除）。

### 9.1 `struct Pop3Mail` —— "邮箱快照"里的一封邮件

登录成功后，服务器把该用户目录下所有 `.eml` 一次性读进内存做成快照，`STAT/LIST/RETR/DELE`
都对着快照操作，不需要反复读磁盘。

| 字段 | 类型 | 含义 |
|---|---|---|
| `path` | `std::string` | 完整文件路径，如 `./mailbox/bob/1788314490_846930886.eml` |
| `size` | `long long` | 文件字节数（`STAT`/`LIST` 的 octets 就是它） |
| `deleted` | `bool` | 本会话是否已被 `DELE` **标记**删除（只是标记！QUIT 才真删文件） |

### 9.2 `struct Pop3State` —— 一次 POP3 会话的状态

| 字段 | 类型 | 含义 |
|---|---|---|
| `username` | `std::string` | `USER` 传来的登录名**原文**（如 `bob@example.com`），用于打日志 |
| `userKey` | `std::string` | 规范化后的用户名（小写、只留 `@` 前部分），用来**找目录 / 查账号** |
| `authed` | `bool` | `PASS` 是否通过（默认 `false`；通过后进入事务态） |
| `mails` | `std::vector<Pop3Mail>` | 当前用户的邮箱快照（会话内编号 `1..N` 固定不变） |

### 9.3 `class Pop3Server` 类总览

| 类型 | 成员 | 说明（详见下文） |
|---|---|---|
| 私有成员 | `std::map<std::string,std::string> accounts_` | 账号表：规范化用户名 → 密码 |
| 公开 | `Pop3Server(int port)` | 构造函数：确保 `./mailbox` 存在 + 加载账号表 |
| 公开 | `~Pop3Server()` | 默认析构 |
| 保护 | `void handleClient(int)` | 重写：处理一个 POP3 客户端会话 |
| 私有 | `bool sendAll(int, const char*, size_t)` | 同 SMTP：保证发完 |
| 私有 | `void sendResponse(int, const std::string&)` | 发一行响应（自动补 `\r\n`） |
| 私有 | `void loadAccounts()` | 读 `./users.txt` 填 `accounts_`（带内置默认账号兜底） |
| 私有 | `std::string normalizeUser(const std::string&) const` | 用户名规范化：小写 + 去 `@` 域名 |
| 私有 | `std::string mailboxDir(const std::string&) const` | 由 userKey 拼收件目录路径 |
| 私有 | `void loadUserMails(const std::string&, std::vector<Pop3Mail>&)` | 扫描目录生成邮箱快照 |
| 私有 | `bool processCommand(int, const std::string&, Pop3State&)` | **状态机核心** |
| 私有(static) | `void splitCommand(const std::string&, std::string&, std::string&)` | 一行拆成 命令+参数 |
| 私有(static) | `int parseMsgNum(const std::string&)` | 参数解析成消息编号 |
| 私有 | `bool isValidMsg(int, const Pop3State&) const` | 编号是否合法且未删除 |
| 私有 | `void statMailbox(const Pop3State&, int&, long long&) const` | 统计未删邮件数与总字节 |
| 私有 | `void sendMailContent(int, const std::string&)` | RETR 时按点填充规则发文件内容 |

### 9.4 `Pop3Server::Pop3Server(int port)` —— 构造函数

| 项目 | 内容 |
|---|---|
| 输入参数 | `port`（int）：POP3 监听端口 |
| 返回值 | 无 |
| 行为 | ① 端口交给基类 `Server`；② `mkdir("./mailbox")`（幂等）；③ `loadAccounts()` 加载账号表 |

### 9.5 `void Pop3Server::loadAccounts()` —— 加载账号表

| 项目 | 内容 |
|---|---|
| 输入参数 | 无 |
| 返回值 | 无（结果写入成员 `accounts_`） |
| 行为 | ① 先塞内置默认账号 `bob/123456`、`alice/123456`（保证第一次能跑）；② 尝试打开 `./users.txt`：逐行解析，去掉行尾 `\r`、首尾空白，跳过空行与 `#` 注释，按第一个 `:` 拆成 `用户名:密码`，用户名经 `normalizeUser()` 规范化后存入临时表；③ 文件里至少有一个有效账号就**整体替换**默认账号，否则保留默认并打印提示 |

### 9.6 `std::string Pop3Server::normalizeUser(const std::string& input) const` —— 用户名规范化

| 项目 | 内容 |
|---|---|
| 输入参数 | `input`：客户端提交的登录名或账号表里的用户名 |
| 返回值 | 规范化结果：`"bob@example.com"→"bob"`、`"Bob"→"bob"`、`" alice "→"alice"`；全是空白则返回空串 |
| 规则 | ① 去首尾空白；② 若含 `@`，只取 `@` 之前的部分；③ 全部转小写 |
| 目的 | 让 `bob` / `Bob` / `bob@example.com` 都能对上**同一个**目录和账号，避免大小写不一致收不到信 |

### 9.7 `std::string Pop3Server::mailboxDir(const std::string& userKey) const` —— 拼收件目录

| 项目 | 内容 |
|---|---|
| 输入参数 | `userKey`：规范化用户名（如 `bob`） |
| 返回值 | `"./mailbox/" + userKey`（如 `./mailbox/bob`） |

### 9.8 `void Pop3Server::loadUserMails(const std::string& userKey, std::vector<Pop3Mail>& mails)` —— 生成邮箱快照

| 项目 | 内容 |
|---|---|
| 输入参数 | `userKey`：规范化用户名；`mails`（引用，出参）：调用前会被 `clear()`，函数结束后是完整的邮件快照 |
| 返回值 | 无 |
| 行为 | ① `opendir(mailboxDir(userKey))`；目录不存在 = 该用户从没收到过信，按**空邮箱**处理直接返回；② 收集所有以 `.eml` 结尾的文件名；③ `std::sort` **按文件名排序**（文件名前缀是时间戳，排序后即接近收信先后，保证每次会话第 N 封固定）；④ 逐个 `stat()` 拿到文件大小，跳过非常规文件（如子目录），组装成 `Pop3Mail` 填入快照 |

### 9.9 `bool Pop3Server::sendAll / void sendResponse` —— 底层收发

与 SMTP 版完全相同的实现（循环 send 保证发完；响应补 `\r\n` 再发），日志前缀为 `[POP3]`。见 8.4 / 8.5。

### 9.10 `void Pop3Server::splitCommand(const std::string& line, std::string& cmd, std::string& args)`（static）—— 拆命令

| 项目 | 内容 |
|---|---|
| 输入参数 | `line`：客户端一行（已去 `\r\n`）；`cmd` / `args`（引用，出参）：拆出的命令与参数 |
| 返回值 | 无 |
| 行为 | 找第一个空格，之前是 `cmd`、之后是 `args`；`args` 开头的多余空格/制表符全部删掉；`cmd` 统一转大写（POP3 命令不区分大小写）；无参数的命令 `args` 为空串 |

### 9.11 `int Pop3Server::parseMsgNum(const std::string& args)`（static）—— 解析消息编号

| 项目 | 内容 |
|---|---|
| 输入参数 | `args`：命令参数（应为纯数字字符串） |
| 返回值 | 正整数 = 消息编号；`0` = 非法（空串 / 含非数字 / 超出上限，POP3 编号从 1 起，0 正好用作"没解析出来"） |
| 实现 | 逐字符校验并累加，用 `long long` 中间累加防溢出（超过 1 亿直接判 0） |

### 9.12 `bool Pop3Server::isValidMsg(int num, const Pop3State& st) const` —— 编号是否可操作

| 项目 | 内容 |
|---|---|
| 输入参数 | `num`：客户端给的消息编号；`st`：会话状态 |
| 返回值 | `true` = 该编号在 `1..mails.size()` 范围内**且未被 DELE 标记**；否则 `false` |

### 9.13 `void Pop3Server::statMailbox(const Pop3State& st, int& count, long long& totalBytes) const` —— 统计邮箱

| 项目 | 内容 |
|---|---|
| 输入参数 | `st`：会话状态（含快照）；`count`（引用，出参）：未删除邮件数；`totalBytes`（引用，出参）：未删除邮件总字节数 |
| 返回值 | 无 |
| 行为 | 遍历快照，跳过 `deleted == true` 的项，累加数量与字节 |

### 9.14 `void Pop3Server::sendMailContent(int fd, const std::string& path)` —— RETR 发邮件内容

| 项目 | 内容 |
|---|---|
| 输入参数 | `fd`：客户端 socket；`path`：要发送的 `.eml` 完整路径 |
| 返回值 | 无 |
| 行为 | ① 以二进制模式打开文件，打不开则回 `-ERR cannot open message file`；② 逐行读（`getline` 按 `\n` 切），去掉行尾残留的 `\r`；③ **点填充**：行首是 `.` 的在前面再加一个 `.`；④ 每行经 `sendResponse` 发出（自动补 `\r\n`，空行也正确）；⑤ 全部发完后发送单独一行的 `.` 表示多行内容结束 |
| 为什么要点填充 | 单独一行的 `.` 是 POP3 多行响应的**结束标记**，若正文真有以 `.` 开头的行而不加转义，客户端会提前误以为内容结束 |

### 9.15 `bool Pop3Server::processCommand(int fd, const std::string& line, Pop3State& st)` —— 命令状态机（核心）

| 项目 | 内容 |
|---|---|
| 输入参数 | `fd`：客户端 socket；`line`：一行命令（已去 `\r\n`）；`st`（引用）：本会话状态（用户名/认证标记/邮箱快照），函数内会修改 |
| 返回值 | `bool`：`true` 继续会话；`false` 会话结束（QUIT） |

**① 空行**：直接忽略。

**② `QUIT`（任何状态下都允许）**：已登录则进入 **UPDATE** 阶段 —— 遍历快照，把所有
`deleted == true` 的邮件用 `unlink()` **真正删除**（逐封打印日志），随后回
`+OK MailForge POP3 server signing off` 并返回 `false`。

**③ 认证态（`!st.authed`）**：

| 命令 | 处理 | 响应 |
|---|---|---|
| `USER <名>` | 保存原文到 `st.username`，规范化到 `st.userKey`；查账号表 | 在表内 `+OK User xxx known`；不在 `-ERR User xxx unknown`；无参数 `-ERR USER requires a mailbox name` |
| `PASS <密码>` | 必须先 USER；查表比对密码 | 匹配：`authed=true`，`mkdir` 确保收件目录存在，`loadUserMails` 读快照，回 `+OK mailbox ready, N message(s), X bytes`；不匹配：清空用户名、回 `-ERR invalid password` |
| 其它 | 事务态命令在认证态不合法 | `-ERR not authenticated, send USER and PASS first` |

**④ 事务态（已登录）**：

| 命令 | 处理 | 响应 |
|---|---|---|
| `STAT` | 调 `statMailbox` | `+OK N X` |
| `LIST` | 无参数：先发总览行 `+OK N message(s), X bytes`，再逐封发 `编号 大小`，最后单独 `.` 结束；带参数：单封 `+OK num size` | 非法编号 `-ERR no such message` |
| `RETR <编号>` | 校验编号 → 回 `+OK size octets` → `sendMailContent` | 非法编号 `-ERR no such message` |
| `DELE <编号>` | 只把快照里那封的 `deleted` 置 `true`（打标记） | `+OK message N deleted`；已删/非法 `-ERR no such message (or already deleted)` |
| `RSET` | 把快照里所有 `deleted` 复位为 `false`（后悔药） | `+OK all delete marks cleared` |
| `NOOP` | 什么都不做（保活防超时掐线） | `+OK` |
| `USER`/`PASS` | 已登录再认证没意义 | `-ERR already authenticated` |
| 其它 | — | `-ERR unknown command` |

### 9.16 `void Pop3Server::handleClient(int client_fd)` —— 一个 POP3 会话的完整生命周期

| 项目 | 内容 |
|---|---|
| 输入参数 | `client_fd`：客户端 socket |
| 返回值 | 无 |
| 行为 | ① 发问候 `+OK MailForge POP3 server ready`；② 创建本会话状态 `Pop3State st`；③ 行缓冲循环（与 SMTP 完全一致：`recv` 4KB → 按 `\n` 拆行 → 去 `\r` → `processCommand`）；④ 客户端断开或 `QUIT` 时关 socket 结束线程 |
| 注意 | 断开连接 ≠ QUIT：**被 DELE 标记但没发 QUIT 就断线的邮件不会真删**，这是 POP3 协议防误删的设计 |

---

## 10. 代码详解 —— 程序入口（`MailServer/main.cpp`）

### 10.1 `int main()` —— 程序入口

| 项目 | 内容 |
|---|---|
| 输入参数 | 无（不解析命令行参数） |
| 返回值 | `int`（正常永不返回：两个服务器都是死循环） |
| 行为 | ① 构造 `SmtpServer smtpServer(2525)` 与 `Pop3Server pop3Server(1110)`；② 打印启动横幅；③ `std::thread pop3Thread(...)` 让 POP3 在**子线程**跑 `start()`；④ 主线程调用 `smtpServer.start()` 阻塞在 SMTP 的 accept 循环；⑤ 正常流程执行不到最后的 `pop3Thread.join()` |
| 为什么分两个线程 | `start()` 是阻塞死循环，想让两个服务器"同时"跑，就必须各占一条线程 |

---

## 11. 账号文件与数据文件说明

### 11.1 `MailServer/users.txt` —— POP3 账号表（纯文本）

| 项目 | 内容 |
|---|---|
| 位置 | 与 `mail_server` 同一运行目录下（程序用相对路径 `./users.txt` 打开） |
| 格式 | 一行一个 `用户名:密码`；`#` 开头是注释；空行忽略 |
| 解析规则 | 用户名会自动"转小写 + 去掉 `@` 域名"，所以写 `bob`、`Bob`、`bob@example.com` 效果一样（见 `normalizeUser`） |
| 与收件目录的关系 | 用户名对应 `./mailbox/<用户名>/`；新增用户在文件里加一行即可，第一次被投递邮件时目录自动创建 |
| 文件缺失/全空 | 程序会**兜底**使用内置默认账号 `bob/123456`、`alice/123456`，并在启动日志中提示 |

当前内容：

```text
bob:123456
alice:123456
```

### 11.2 邮箱数据目录 `MailServer/mailbox/`

```
mailbox/
├── bob/                        # 用户名 = 收件人@前面的部分（小写）
│   ├── 1788314452_1804289383.eml
│   └── 1788314490_846930886.eml
└── alice/                      # 用户第一次 POP3 登录成功后自动创建（可空）
```

- SMTP 收信 → 按收件人投到对应子目录；
- POP3 登录 → 扫描对应用户的子目录生成快照；
- 文件是标准 `.eml`，可直接用文本编辑器 / 邮件客户端打开查看。

---

## 12. 仓库内其余代码说明（非当前主线）

### 12.1 `连接测试/` —— 里程碑 1 的练手代码（HTTP 连通性服务器）

早期学习 Socket 时写的**单线程** TCP/HTTP 服务器，用来验证浏览器能连上 C++ 后端。
与 MailServer 无关，保留作学习参考。其中包含：

**`utils.h / utils.cpp`**

| 条目 | 内容 |
|---|---|
| 宏 `SERVER_PORT 8888` | 练手服务器的监听端口 |
| 宏 `BUFFER_SIZE 4096` | `recv()` 单次读取上限 |
| `std::string trim(const std::string& str)` | **输入**：任意字符串；**返回**：去掉首尾空白（空格/Tab/回车/换行）后的字符串；整串全是空白时返回 `""`。实现用 `find_first_not_of` + `find_last_not_of` 定位非空白区间后 `substr` 截取 |

**`server.cpp`** —— 单线程连通性服务器

| 函数 | 输入参数 | 返回值 | 作用 |
|---|---|---|---|
| `void handle_client(int client_fd)` | 客户端 socket | 无 | `recv` 收请求并打印 → 拼一段写死的 HTML → 按 HTTP/1.1 响应格式发回（含 `Content-Length`）→ `close` |
| `int main()` | 无 | 0 | `socket → setsockopt → bind(端口3225) → listen → while(accept)`，每收到一个连接就同步调用 `handle_client`（单线程，串行处理） |

> 注：该文件里宏 `PORT` 定义为 3225（与头文件里的 8888 无关，因为是两段独立练手代码）。

### 12.2 `SmtpClientWeb/` —— 早期浏览器发信联调残留

目录里只有编译产物 `bridge`（ELF 可执行文件），**仓库内没有它的源码**，不属于当前主线，
可视为早期"浏览器 → bridge → SMTP"联调尝试的遗留物。

### 12.3 `setup_portproxy.bat` —— Windows 端口转发脚本

用于 WSL2 环境：自动读取 WSL 内部 IP，在 Windows 上添加 `0.0.0.0:端口 → WSL:端口` 的
`netsh portproxy` 转发并放行防火墙，让局域网 / ZeroTier 设备能访问到跑在 WSL 里的服务器。

### 12.4 `requirements.md` —— 课程需求文档

原始课程需求（SMTP/POP3 收发、加密、性能指标等），与当前代码的**对照状态**：
协议收发部分已完成，加密与 Web 端为后续里程碑。

---

## 13. 已修复的 Bug 记录（重要）

在把整套代码跑通的过程中，修复了以下会导致服务器无法工作的问题：

| # | 位置 | 问题 | 影响 | 修复 |
|---|---|---|---|---|
| 1 | `Server::start()` | `socket()` 成功后多写 `else { return true; }` | 后面的 bind/listen/accept 全是死代码，**服务器"秒退"、不监听端口** | 删除提前 `return`，让流程继续 |
| 2 | `Server::start()` | `setsockopt(..., sizeof(opt < 0))` 笔误 | 传了 `bool` 的长度 1 而不是 `int` 的 4（部分平台会失效） | 改为 `sizeof(opt)` |
| 3 | `SmtpServer::processCommand()` | `if(dataMode)` 的两个分支都 `return true`，且把点填充还原写在非 DATA 分支 | 后边的 HELO/MAIL/RCPT/DATA/QUIT 全部**不可达**，服务器只回 220 不应答任何命令 | 还原逻辑移进 DATA 模式的 else 分支；非 DATA 时继续往下解析命令 |
| 4 | `SmtpServer::processCommand()` | `MAIL` 分支结束后悬空一个 `return true;` | `RCPT / DATA / QUIT` 全部**失效**（发信卡在 RCPT） | 把 `return true` 收进 `if(cmd=="MAIL")` 块内 |
| 5 | `SmtpServer::processCommand()` | HELO 里 `else args;` 是无用语句 | 问候语里主机名恒为空 | 改为 `else domain = args;` |
| 6 | `SmtpServer::saveMail()` | 所有邮件平铺存 `./mailbox/` | 无法按用户隔离，POP3 无意义 | 改为按收件人投递到 `./mailbox/<用户名>/`（新功能） |

---

## 14. 测试情况

开发完成后用 Python 标准库 `smtplib`（SMTP 客户端）、`poplib`（POP3 客户端）以及裸 socket
做了**端到端协议测试，24 项全部通过**，覆盖：

- 认证：错误密码拒绝、未登录访问事务命令拒绝、`bob@example.com` 带域名登录也能识别；
- 收信：`STAT / LIST / LIST n / RETR`（含中文正文）与各种非法编号的 `-ERR` 分支；
- 删除语义：`DELE` 只打标记 → `STAT/LIST/RETR` 立刻反映 → `RSET` 反悔 → 断线（不发 QUIT）不删文件 → **QUIT 才真正删除**；
- 协议细节：POP3 **点填充**（正文 `.` 开头行在网络上发成 `..`）；
- 多用户隔离：`alice` 登录看不到 `bob` 的邮件；
- 全链路：`SMTP 发信 → 自动投递到 ./mailbox/bob/ → POP3 登录收取`。

复测命令：

```bash
cd MailServer
./mail_server &                 # 先启动服务器
python3 /tmp/mail_test.py       # 端到端测试脚本（本机开发时用，见下）
```

> `/tmp/mail_test.py` 是开发期间编写的端到端测试脚本（使用 Python 标准库 `smtplib` /
> `poplib` + 裸 socket），不属于仓库源码；你也可以用 Python 交互式自行验证，例如：
> `python3 -c "import poplib; p=poplib.POP3('127.0.0.1',1110); p.user('bob'); p.pass_('123456'); print(p.stat()); p.quit()"`

---

## 15. 常见问题（FAQ）

**Q1：为什么不用 25/110 而用 2525/1110？**
Linux 上 25/110 是特权端口，需要 root；且常被运营商/防火墙拦截。2525/1110 是高端口，本地开发测试最方便。

**Q2：POP3 收完信，服务器上的邮件会被删掉吗？**
不会。本实现里只有客户端显式发 `DELE` 且正常 `QUIT` 才删文件；只 `RETR`（下载）不删信。对将来的 Web 邮箱很友好。

**Q3：怎么新增一个用户？**
在 `users.txt` 加一行 `新用户名:密码`，并重启服务器（`loadAccounts` 只在启动时执行一次）。之后 SMTP 首次给该用户投递邮件时会自动创建 `./mailbox/<用户名>/`。

**Q4：用户名大小写 / 带域名能登录吗？**
能。`USER Bob`、`USER bob@example.com`、`USER bob` 都会被规范化成 `bob` 再匹配账号和目录。

**Q5：邮件都存到哪了？长什么样？**
`./mailbox/<用户名>/时间戳_随机数.eml`，内容是标准 RFC 5322 邮件（头部区 + 空行 + 正文），普通文本编辑器可直接打开。

**Q6：将来 Web 邮箱系统怎么和它对接？**
浏览器不能直接连 2525/1110（浏览器只有 HTTP）。正确做法：你的 C++ Web 后端再写一份
**SMTP 客户端**与 **POP3 客户端**，内部连接本机的 2525/1110 完成收发，再通过 HTTP 接口暴露给前端。
（这也正是 README 规划中 `src/mail/mail_client.*` 要做的事。）

**Q7：users.txt 什么时候会重新读取？**
只在 `Pop3Server` 构造（服务器启动）时读一次。改了账号需要重启进程。

---

## 16. 后续规划（对照需求清单）

- [x] SMTP 协议（EHLO/MAIL FROM/RCPT TO/DATA/QUIT）
- [x] POP3 协议（USER/PASS/STAT/LIST/RETR/DELE/QUIT）
- [x] 多用户独立邮箱目录、EML 落盘持久化
- [ ] SMTP 客户端 + POP3 客户端（C++，供 Web 后端调用）
- [ ] 邮件加密模块（≥2 种对称算法）
- [ ] C++ HTTP 服务器 + Web 前端（B/S 架构收发界面）
- [ ] 性能压测：100 次收发成功率 ≥ 99%

---

*本文档由仓库当前代码整理生成，如代码有改动请同步更新。*

