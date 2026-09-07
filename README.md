# 邮件协议实现及应用系统研制（C/C++）

计算机与网络课程设计大作业 —— **邮件加密** 子系统

## 一、任务要求

| 类别 | 要求 |
|------|------|
| 基线 | 纯 Socket API 手写 SMTP（发送）与 POP3（接收），禁止 libcurl/VMime 等现成库 |
| 提升 | 邮件正文加密：≥2 种算法协同（AES-256-CBC + RSA-2048 混合加密，即数字信封） |
| 性能 | 1MB 邮件回环往返 <2s；100 次连续收发成功率 ≥99% |
| 技术栈 | C++17，OpenSSL 3.0，C/S 架构，Linux（WSL2）/Windows 均可 |

## 二、系统架构

```
┌─────────────────────┐   SMTP协议(手写)   ┌──────────────────────┐
│     邮件客户端        │  ────────────────► │     邮件服务器         │
│  ┌────────────────┐  │     TCP :25       │  ┌──────────────────┐ │
│  │ 数字信封        │  │                   │  │ SMTP处理器(状态机) │ │
│  │ AES加密正文     │  │   POP3协议(手写)  │  ├──────────────────┤ │
│  │ RSA加密会话密钥 │  │ ◄──────────────── │  │ 邮箱存储(Mailbox)  │ │
│  │ SHA256数字签名  │  │     TCP :110      │  ├──────────────────┤ │
│  └────────────────┘  │                   │  │ POP3处理器(状态机) │ │
└─────────────────────┘                   └──────────────────────┘
```

## 三、目录结构

```
├── common/          # Socket封装(跨平台 POSIX/Winsock) / Base64 / 日志
├── crypto/          # RSA-2048 / AES-256-CBC / 数字信封 / 密钥管理(PEM)
├── protocol/        # smtp_client / pop3_client / smtp_server / pop3_server
├── server/          # 服务器入口(main) + 邮箱存储
├── client/          # 客户端入口(main) + 命令行交互
├── tests/           # 加密自测 + 100次压力测试(性能验收)
├── keys/            # 运行时生成的密钥文件(勿提交到 git)
└── Makefile         # 构建脚本
```

## 四、实施路线图（进度追踪）

- [x] Step 0: 项目骨架 + 方案规划
- [x] Step 0b: WSL2 + Ubuntu + g++ + OpenSSL 3.0 环境就绪
      - [x] MSYS2 临时编译台（Windows 本机）：g++ 16.2.0 + OpenSSL 3.6.4 + GNU make ✅
      - [x] WSL2 + Ubuntu-22.04 正式环境：g++ 11.4.0 + GNU Make 4.3 + OpenSSL 3.0.2 + libssl-dev ✅（make all 全量构建通过；base64/logger/crypto/socket 四套自测全绿；修复 socket.cpp POSIX 分支缺 <netdb.h> 导致 addrinfo/getaddrinfo 无法链接的跨平台 bug）
- [ ] Step 1: 公共模块（Socket 跨平台封装 / Base64 / 日志）→ 自测
      - [x] Base64 模块（common/base64.hpp/.cpp）+ 测试（tests/test_base64.cpp）—— ✅ 25项测试通过
      - [x] 日志模块（common/logger.hpp/.cpp）+ 测试（tests/test_logger.cpp）—— ✅ 多线程测试通过
      - [x] Socket 跨平台封装（common/socket.hpp/.cpp，POSIX/Winsock）+ 测试（tests/test_socket.cpp）—— ✅ 环回 26 项自测通过（监听/连接/行收发/拆包拼接/超时/对端关闭/200KB大数据/拒绝连接/localhost解析）
- [ ] Step 2: SMTP 协议（服务器状态机 + 客户端）→ 明文发送落盘
- [ ] Step 3: POP3 协议（服务器状态机 + 客户端）→ 明文收发闭环
- [ ] Step 4: 加密模块（RSA / AES / 数字签名）→ 单元测试
      - [x] OpenSSL 工具层（crypto/openssl_util.hpp/.cpp）—— RAII + 随机数 + 错误处理
      - [x] AES-256-CBC（crypto/aes.hpp/.cpp）—— ✅ NIST SP800-38A 向量匹配 + 往返测试
      - [x] RSA-2048（crypto/rsa.hpp/.cpp）—— ✅ 密钥生成/PEM存取/加解密/签名验签
      - [x] 数字信封（crypto/envelope.hpp/.cpp）—— ✅ Seal/Open + ASCII序列化 + 防篡改
      - [x] 加密测试（tests/test_crypto.cpp）—— ✅ 44 项全部通过
      - [x] 可视化测试工具（tests/demo_visual.cpp）—— 密钥指纹/明密文对比/篡改演示/性能基准
      - [x] 命令行接口（tools/crypto_cli.cpp）—— 加密模块 CLI，供 GUI/脚本/集成调用
      - [x] tkinter 图形界面（tools/mail_crypto_gui.py）—— 生成密钥/加密/解密/篡改/性能 一键演示
- [ ] Step 5: 数字信封集成 → 加密发送 / 解密接收
- [ ] Step 6: 多线程服务器 + 100 次压力测试（性能验收）
- [ ] Step 7: 打磨收尾（异常处理 / 日志 / 演示 / 答辩要点）

## 五、构建方式（WSL2 / Linux）

```bash
cd /mnt/c/Users/DELL/Desktop/计算机网络作业
make all     # 编译全部
make test    # 运行自测
make stress  # 100次压力测试
```

## 六、数字信封格式（加密邮件的载体）

```
-----BEGIN MAIL ENVELOPE-----
Version: 1.0
Cipher:   AES-256-CBC
EncKey:   <Base64：RSA-2048加密后的AES会话密钥>
IV:       <Base64：AES初始化向量>
Signature:<Base64：发件人RSA私钥对SHA-256摘要的签名>
From:     alice@test.com
To:       bob@test.com
Body:     <Base64：AES-256-CBC加密后的正文密文>
-----END MAIL ENVELOPE-----
```

> 头部（From/To）保留明文以便服务器路由；正文与密钥均加密；签名保证完整性与身份认证。
