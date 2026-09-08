# MailForge —— 邮件服务器（SMTP + POP3 + Web + 端到端加密）

> 计算机与网络课程设计 —— 选题 18：邮件协议实现及应用系统研制（C/C++）

MailForge 是一个从零实现的课程级邮件系统：

- 原生 **Socket 编程**手写 **SMTP（RFC 5321）发送** 与 **POP3（RFC 1939）收取** 协议，不依赖任何现成协议库；
- 自带 **Web 收发界面**（HTTP 服务器手写 REST 接口）与**多用户邮箱隔离**（邮件按 `.eml` 标准格式落盘，重启不丢失）；
- 集成**数字信封加密**（AES-256-CBC + RSA-2048，OpenSSL 3.0）——加密子系统 `crypto/`、`common/` 由搭档 zuumm594-art 开发后并入本仓库，实现从浏览器到收件人的**端到端加密**。

---

## 一、功能特性

| 能力 | 说明 |
|---|---|
| SMTP 发信 / POP3 收信 | 完整协议状态机，服务端口 2525 / 1110 |
| 多用户邮箱 | 每个账号独立收件目录 `./mailbox/<用户名>/`，账号表 `users.txt` |
| Web 界面 | 注册 / 登录 / 写邮件 / 收件箱 / 阅读 / 删除 / 附件上传下载，端口 8080 |
| 邮件附件 | MIME multipart，可与加密叠加（附件随正文一起封进信封） |
| 端到端加密 | **≥2 种可切换算法**：数字信封（AES-256-CBC + RSA-2048 + SHA-256 签名）或 AES-256-CBC 对称，下拉选择即加密 |
| 协议演示终端 | `demo_smtp.html` / `demo_pop3.html`：在浏览器里像 telnet 一样手敲命令 |
| 性能压测 | 网页一键连发 100 封 1MB+ 附件邮件，统计成功率 / 平均时延 / 丢包率 |

---

## 二、架构总览

```
浏览器 ──HTTP──► HttpServer(8080) ──┐
   ▲                              │ 扮演 SMTP/POP3 客户端
   │                              ▼
 Web 页面 / REST JSON       SmtpClient ──发信──► SMTP(2525)
                            Pop3Client  ──收信──► POP3(1110)
                                                    │
                                                    ▼
                                          ./mailbox/<用户名>/xxx.eml
```

- 三个服务器共用网络基类 `Server`（socket → bind → listen → accept → 每连接一线程），协议子类只需实现 `handleClient`。
- SMTP 投递、POP3 读取、Web 收发都围绕同一份 `.eml` 存储，天然一致。
- **加密与协议解耦**：加密发生在 HTTP 层的发送前 / 收取后（`MailCrypto` 模块）；SMTP/POP3 服务器对密文完全透明，所以加密通道与非加密通道、以及外部标准邮件客户端（Outlook/python）收发都不受影响。

## 三、目录结构

```
MailForge/
├── Makefile              # 统一构建（见「快速开始」）
├── README.md             # 本文档
├── TEST_REPORT.md        # 测试报告（验收留证：回归 25/25、100 封压测数据）
├── requirements.md       # 课程需求原文与验收清单（已全部达标）
├── LICENSE
├── tests/                # 端到端回归脚本 e2e_regression.py（make test-e2e）
├── MailServer/           # ★ 主项目：邮件服务器 + Web 后端
│   ├── main.cpp          #   程序入口：同时启动 SMTP + POP3 + HTTP
│   ├── include/ src/     #   协议 / HTTP / 加密模块源码
│   ├── web/              #   前端页面（index / demo_smtp / demo_pop3）
│   ├── client_test.cpp   #   SMTP/POP3 协议客户端演示程序
│   ├── users.txt         #   默认账号 bob、alice（密码 123456）
│   ├── setup_portproxy.bat # Windows 局域网/ZeroTier 访问端口转发脚本
│   ├── keys/             #   RSA 密钥（运行时生成，勿提交）
│   └── mailbox/          #   邮件落盘目录（运行时生成，勿提交）
├── crypto/               # 加密子系统（mail:: 命名空间，OpenSSL）
│   ├── aes / rsa / envelope / openssl_util  # AES-256-CBC / RSA-2048 / 数字信封
│   ├── tests/            #   单元自测（test_crypto 44 项等）
│   ├── tools/            #   命令行加密工具 + tkinter 图形演示
│   ├── docs/             #   加密学习文档
│   └── practice/         #   信封练习代码
└── common/               # 加密子系统公共库：base64 / logger / file_util / socket
```

## 四、快速开始

环境：Linux（或 WSL2 / MSYS2） + `g++`（C++17） + `make` + OpenSSL 3.0（`libssl-dev`）。

```bash
make server        # 编译服务器（含加密子系统），产物 MailServer/mail_server
make run           # 编译并启动：SMTP 2525 / POP3 1110 / HTTP 8080
make client        # 编译 SMTP/POP3 客户端演示（可选）
make test-crypto   # 运行加密子系统自测
make test-e2e      # 端到端回归：协议/HTTP/加密/附件/协议终端 25 项一键复现
make clean         # 清理全部产物
```

启动后在浏览器打开 **http://localhost:8080**，用 `bob` / `alice`（密码均为 `123456`）登录即可收发。

## 五、端到端加密（数字信封）

MailForge 的默认加密通道是**数字信封**，由加密子系统（`crypto/`）实现：

1. 写邮件时在「加密方式」下拉框选择（或调用 `/api/send` 时带 `encrypt` 参数）：
   - `encrypt=1`：数字信封（AES-256-CBC + RSA-2048 + SHA-256 签名，**推荐**）；
   - `encrypt=2`：AES-256-CBC 对称通道（第二档，满足"≥2 种可切换算法"要求）。
2. 以数字信封为例：发送端把「真实主题 + 正文（含附件 multipart）」打包，用**收件人的 RSA 公钥**封成信封：
   - AES-256-CBC 加密正文内容；
   - RSA-2048 加密随机生成的 AES 会话密钥（公钥加密，只有收件人私钥能解）；
   - 发件人 RSA 私钥对密文做 SHA-256 签名（防篡改 + 身份认证）。
3. SMTP 传输、服务器落盘、POP3 下载全程只看到一段 ASCII 信封文本，**看不到任何明文**；
4. 收件人阅读时用**自己的私钥**自动拆封，并用发件人公钥验签，正文与主题原样还原。

密钥管理是零配置的：每个用户在 **POP3 登录成功时**自动生成一对 RSA-2048 密钥，存放在
`MailServer/keys/`（`<用户名>.key.pem` 私钥、`<用户名>.pub.pem` 公钥）。历史版本用 XOR
通道发出的邮件仍可正常读取（向后兼容）。

信封的 ASCII 落地格式示例（`.eml` 正文区）：

```text
-----BEGIN MAIL ENVELOPE-----
Version: 1.0
Cipher: AES-256-CBC
From: alice@example.com
To: bob@example.com
EncKey: <Base64：RSA-2048 加密的 AES 会话密钥>
IV: <Base64：AES 初始化向量>
Signature: <Base64：发件人签名>
Body: <Base64：AES-256-CBC 密文，每 76 字符换行>
-----END MAIL ENVELOPE-----
```

## 六、端口与账号约定

| 服务 | 协议 | 端口 | 说明 |
|---|---|---|---|
| SMTP 服务器 | SMTP | **2525** | 邮件发送入口（程序间收发用，25 需 root 且常被防火墙拦） |
| POP3 服务器 | POP3 | **1110** | 邮件收取入口 |
| HTTP 服务器 | HTTP | **8080** | 浏览器唯一入口（Web 页面 + REST） |

默认账号：`alice` / `bob`，密码均为 `123456`。账号存于 `MailServer/users.txt`（`用户名:密码` 每行一个），
网页注册的新账号即时生效，无需重启。

## 七、测试情况

- **协议端到端**：SMTP 发信 → 自动投递 `./mailbox/bob/` → POP3 登录收取，含中文、附件、点填充、删除语义、多用户隔离等场景。
- **加密端到端**：加密发送 → 落盘确认为 ASCII 信封（主题/正文/附件明文均未泄漏）→ 收件人自动拆封还原 → 附件下载字节与发送完全一致；明文通道不受影响。
- **性能压测**：连发 100 封 ~1MB 附件邮件：发送 100/100、丢包率 0%、平均单封约 40ms（网页 `/api/benchmark` 可复现，测完自动清理）。
- **加密子系统自测**：`make test-crypto` —— base64 / logger / socket / crypto 四套共 100+ 项全部通过。
- **一键端到端回归**：`make test-e2e` —— 25 项断言全部通过（协议层 / HTTP 三层加密 / 附件 / 协议终端 / 多用户隔离），脚本自动启停服务器并清理环境。

完整测试数据与 100 封性能压测记录见 **`TEST_REPORT.md`**。复测方法：先 `make run`
启动服务器，然后网页操作；协议层可用 Python `smtplib` / `poplib` 或
`./MailServer/mail_client_test` 复测。

---

## 八、演示入口一览

| 地址 | 说明 |
|---|---|
| `http://localhost:8080/` | Web 邮箱主界面（登录 / 收发 / 加密 / 附件 / 压测） |
| `http://localhost:8080/demo_smtp.html` | SMTP 协议演示终端（浏览器手敲命令） |
| `http://localhost:8080/demo_pop3.html` | POP3 协议演示终端 |
| `crypto/tools/crypto_cli` | 加密子系统命令行工具（`make demo-crypto` 后可用） |

局域网 / ZeroTier 演示：运行 `MailServer/setup_portproxy.bat` 配置 Windows 端口转发后，
用 `http://<本机IP>:8080` 访问。

课程需求细节与逐项对照见 `requirements.md`；加密子系统学习材料见 `crypto/docs/crypto_study_guide.md`。
