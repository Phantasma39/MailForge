# MailForge —— 邮件服务器（SMTP + POP3 + Web + 端到端加密）

> 计算机与网络课程设计 —— 选题 18：邮件协议实现及应用系统研制（C/C++）

MailForge 是一个从零实现的课程级邮件系统：

- 原生 **Socket 编程**手写 **SMTP（RFC 5321）发送** 与 **POP3（RFC 1939）收取** 协议，不依赖任何现成协议库；
- 自带 **Web 收发界面**（HTTP 服务器手写 REST 接口）与**多用户邮箱隔离**（邮件按 `.eml` 标准格式落盘，重启不丢失）；
- 集成**两层加密**：① Web↔服务器 用 RSA 数字信封（AES-256-GCM + RSA-OAEP-2048，服务器端 OpenSSL 库、浏览器端原生 WebCrypto）；② 服务器端口间/落盘 用**纯 C++ 自研对称加密**（AES-256-CBC + ChaCha20，RFC 8439，不调库），邮件在 SMTP/POP3 传输与 `.eml` 存储中只有密文。

---

## 一、功能特性

| 能力 | 说明 |
|---|---|
| SMTP 发信 / POP3 收信 | 完整协议状态机，服务端口 2525 / 1110 |
| 多用户邮箱 | 每个账号独立收件目录 `./mailbox/<用户名>/`，账号表 `users.txt` |
| Web 界面 | 注册 / 登录 / 写邮件 / 收件箱 / 阅读 / 删除 / 附件上传下载，端口 8080 |
| 邮件附件 | MIME multipart，可与加密叠加（附件随正文一起封进信封） |
| 已发送 | 发信成功自动保存副本到 `./sent/<账号>/`；加密邮件以**发件人本人密钥**再存一份，支持列表 / 阅读 / 附件下载 / 删除 |
| 邮件加密 | **两层**：① Web↔服务器 RSA 信封（AES-256-GCM + RSA-OAEP，OpenSSL/WebCrypto）；② 服务器内部端口间自研对称加密 **AES-256-CBC** / **ChaCha20**（RFC 8439，可切换、无 OpenSSL），SMTP/POP3 传输与 `.eml` 落盘均为密文 |
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
- **加密与协议解耦（两层模型）**：
  - 层1 Web↔HTTP(8080)：浏览器把发送表单封成 RSA 信封（AES-256-GCM + RSA-OAEP-2048）后再提交；服务器返回的收件箱/正文/附件同样以信封返回。服务器端用 OpenSSL 库，浏览器端用原生 WebCrypto，本机无 TLS 时的应用层机密性。
  - 层2 服务器内部（8080↔2525/1110 端口间与落盘）：`MailCrypto` 用收件人账号对称密钥对整封 `.eml` 内容做自研 AES-256-CBC / ChaCha20 加密，SMTP/POP3 服务器对密文完全透明，外部标准邮件客户端收发不受影响。

## 三、目录结构

```
MailForge/
├── Makefile              # 统一构建（见「快速开始」）
├── README.md             # 本文档
├── requirements.md       # 课程需求原文与实现对照
├── LICENSE
├── MailServer/           # ★ 主项目：邮件服务器 + Web 后端
│   ├── main.cpp          #   程序入口：同时启动 SMTP + POP3 + HTTP
│   ├── include/ src/     #   协议 / HTTP / 加密模块源码
│   ├── web/              #   前端页面（index / demo_smtp / demo_pop3）
│   ├── client_test.cpp   #   SMTP/POP3 协议客户端演示程序
│   ├── users.txt         #   默认账号 bob、alice（密码 123456，落盘为 PBKDF2-SHA256 哈希）
│   ├── setup_portproxy.bat # Windows 局域网/ZeroTier 访问端口转发脚本
│   ├── keys/             #   密钥（运行时生成，勿提交）：用户对称密钥 <用户>.key、
│   │                     #   层1 服务器 RSA 密钥 server_public.pem / server_private.pem
│   └── mailbox/          #   邮件落盘目录（运行时生成，勿提交）
├── crypto/               # 加密子系统（mail:: 命名空间）
│   ├── aes / chacha20 / random   # 层2 自研：AES-256-CBC / ChaCha20(RFC 8439) / CSPRNG（无 OpenSSL）
│   ├── rsa / openssl_util        # 层1 Web↔服务器 RSA 信封（OpenSSL 库）
│   ├── tests/            #   单元自测（AES 过 NIST SP800-38A、ChaCha20 过 RFC 8439、RSA 信封往返）
│   └── docs/             #   加密学习文档
└── common/               # 加密子系统公共库：base64 / logger / file_util / socket
```

## 四、快速开始

环境：Linux（或 WSL2）+ `g++`（C++17）+ `make` + **OpenSSL 开发库（libssl-dev）**。
说明：层2 加密算法（AES-256-CBC / ChaCha20）为纯 C++ 自研、**不依赖 OpenSSL**；
层1 Web↔服务器 RSA 信封由服务器端调用 OpenSSL（浏览器端用原生 WebCrypto，无第三方库）。

```bash
make server        # 编译服务器（层1 RSA + 层2 自研 AES/ChaCha），产物 MailServer/mail_server
make run           # 编译并启动：SMTP 2525 / POP3 1110 / HTTP 8080
make client        # 编译 SMTP/POP3 客户端演示（可选）
make test-crypto   # 层2 自研加密自测（AES-256-CBC / ChaCha20 官方向量）
make test-rsa      # 层1 Web↔服务器 RSA 信封单元测试（make server 已含）
make clean         # 清理全部产物
```

启动后在浏览器打开 **http://localhost:8080**，用 `bob` / `alice`（密码均为 `123456`）登录即可收发。

## 五、邮件加密（两层模型）

### 层1：Web ↔ 服务器（8080）—— RSA 数字信封（调用 OpenSSL 库）

浏览器与服务器之间没有 TLS 时，邮件内容由应用层信封保护：

1. 浏览器（**原生 WebCrypto**，无第三方库）生成一次性 AES-256-GCM 会话密钥；
2. 用 **服务器的 RSA-2048 公钥**做 RSA-OAEP(SHA-256) 封装会话密钥，内容（to/主题/正文/附件）用 GCM 加密后提交 `/api/send`；
3. 服务器用 **OpenSSL 私钥**拆封还原，继续走层2；
4. 反向同样：收件箱/邮件正文/附件下载以 **浏览器公钥**封装的信封返回，浏览器私钥解封显示。

密钥零配置：服务器首次启动自动生成 `keys/server_public.pem` / `server_private.pem`；
浏览器密钥对以 JWK 保存在 localStorage，登录后自动上传公钥（`POST /api/webpub`）。

### 层2：服务器内部端口之间（8080 ↔ 2525/1110 与落盘）—— 自研 AES-256-CBC / ChaCha20

1. 写邮件时选择算法（**AES-256-CBC** 或 **ChaCha20**，RFC 8439）并勾选「发送时加密」；
2. 发送端把「真实主题 + 正文（含附件 multipart）」打包，用**收件人账号的对称密钥**（`keys/<用户名>.key`，32 字节随机，首次使用自动生成）整体加密；
3. SMTP 传输、服务器落盘、POP3 下载全程只看到一段密文正文（`MailForge::ENC::AES::` / `MailForge::ENC::CHA::` 头 + Base64），**看不到任何明文**；
4. 收件人阅读时用自己账号的密钥自动解密，正文与主题原样还原。

两个对称算法都是纯自研并通过官方向量回归：**AES-256-CBC 过 NIST SP 800-38A**、
**ChaCha20 过 RFC 8439 §2.3.2**（`make test-crypto`）；层1 信封有独立单元测试
`make test-rsa`（Seal/Open 双向互通 + 防篡改）。密钥管理零配置，登录或首次收发时自动生成。

> 说明：B/S 架构的信任边界在服务器——服务器必须能读到自己用户的内容。层2 保证
> **SMTP/POP3 传输与磁盘存储密文化**；层1 进一步保护**浏览器与服务器之间**的机密性
> （本机无 TLS 时的应用层替代）。每封邮件随机 IV/nonce / 会话密钥，AES-GCM/CBC 均带
> 完整性校验，篡改即解密失败。


## 六、端口与账号约定

| 服务 | 协议 | 端口 | 说明 |
|---|---|---|---|
| SMTP 服务器 | SMTP | **2525** | 邮件发送入口（程序间收发用，25 需 root 且常被防火墙拦） |
| POP3 服务器 | POP3 | **1110** | 邮件收取入口 |
| HTTP 服务器 | HTTP | **8080** | 浏览器唯一入口（Web 页面 + REST） |

默认账号：`alice` / `bob`，密码均为 `123456`。账号存于 `MailServer/users.txt`（`用户名:PBKDF2-SHA256 哈希` 每行一个，旧版明文账号首次启动会自动迁移为哈希），
网页注册的新账号即时生效，无需重启。

## 七、测试情况

- **协议端到端**：SMTP 发信 → 自动投递 `./mailbox/bob/` → POP3 登录收取，含中文、附件、点填充、删除语义、多用户隔离等场景。
- **加密端到端**：加密发送 → 落盘确认正文为 `MailForge::ENC::AES::` / `MailForge::ENC::CHA::` 密文（主题/正文/附件明文均未泄漏）→ 收件人读取自动解密还原；明文通道不受影响。
- **性能压测（含加密）**：网页 `/api/benchmark` 可选 明文 / AES-256-CBC / ChaCha20 / 三种全跑。实测每模式连发 100 封 ~1MB 邮件：发送 100/100、丢包率 0%、加密邮件逐封解密还原 100/100；AES 平均单封约 100ms、ChaCha20 约 60ms（远低于 2s 指标），测完自动清理；支持客户端线程池并发发送（`threads=1/2/4/8/16`）与多账号轮询发件（`multi=1`），可对比单线程与多线程吞吐。
- **加密子系统自测**：`make test-crypto` —— base64 / logger / socket / crypto 全部通过；AES-256-CBC 匹配 NIST SP 800-38A 官方向量，ChaCha20 匹配 RFC 8439 §2.3.2 官方向量。
- **层1 RSA 信封自测**：`make test-rsa` —— 服务器密钥自动生成、浏览器↔服务器双向信封 Seal/Open 一致（含 WebCrypto 等价解封路径）、篡改/错误密钥拒绝，全部通过。

复测方法：先 `make run` 启动服务器，然后网页操作；协议层可用 Python `smtplib` / `poplib` 或
`./MailServer/mail_client_test` 复测。

---

## 八、演示入口一览

| 地址 | 说明 |
|---|---|
| `http://localhost:8080/` | Web 邮箱主界面（登录 / 收发 / 加密算法选择 / 附件 / 压测） |
| `http://localhost:8080/demo_smtp.html` | SMTP 协议演示终端（浏览器手敲命令） |
| `http://localhost:8080/demo_pop3.html` | POP3 协议演示终端 |

局域网 / ZeroTier 演示：运行 `MailServer/setup_portproxy.bat` 配置 Windows 端口转发后，
用 `http://<本机IP>:8080` 访问。

课程需求细节与逐项对照见 `requirements.md`；加密子系统学习材料见 `crypto/docs/crypto_study_guide.md`。
