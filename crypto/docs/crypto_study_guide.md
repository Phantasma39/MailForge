# 加密算法学习路线（对照代码逐层拆解）

> 适用：负责**邮件加密算法**模块的同学
> 目标：把"黑箱"变"白箱"——读完能自己讲清楚一封邮件从 Seal 到 Open 的全部过程。
>
> 阅读原则：**先 .hpp（接口注释=设计意图）→ 再 .cpp（实现）→ 最后用测试验收**。
> 顺序 = 依赖方向：先学被依赖的工具，再学用它的人。

## 全景数据流（先记住这张图）

```
【发送方 Seal】                          【接收方 Open】
 邮件正文(任意长度)                           邮件正文(还原)
      │ AES-256-CBC 加密(快)                    ▲ AES-256-CBC 解密
      │  密钥=随机会话密钥 + IV                 │  (用解出的会话密钥)
      ▼                                        │
   正文密文 ──(SHA-256摘要 + RSA私钥签名)──► 数字签名
      │                                        │ 验签(用发送方公钥)
      ▼                                        ▲
   RSA-OAEP 用"接收方公钥"加密 32B 会话密钥      │ RSA 解密(用自己私钥)
      ▼                                        │
  EncKey + IV + Signature + 密文(Body)          │
      │                                        │
   全部 Base64 → 拼成 ASCII 信封文本 ──────────┘
   （From/To 明文保留，供 SMTP 服务器路由）
```

| 零件 | 为什么必须有 |
|---|---|
| AES-256-CBC（对称） | 加密**快**，能处理任意长度正文 |
| RSA-2048（非对称） | 解决"会话密钥怎么安全送达"（RSA 慢且单次限长 190B） |
| IV 随机 | 同一明文 + 同密钥两次加密结果不同，防止 CBC 模式泄密 |
| SHA-256 签名 | 防篡改 + 身份认证（仅发送方私钥能签出合法签名） |
| Base64 | SMTP 只传 7-bit 文本，二进制必须转 ASCII |

## 文件学习顺序（读完一项打一个勾）

### 阶段一：看懂报错与文件（~20 分钟）
- [ ] 1. `common/logger.hpp` / `logger.cpp` —— 加密代码里的 `[ERROR]` 日志从哪来（宏 + 级别过滤）
- [ ] 2. `common/file_util.hpp` / `file_util.cpp` —— 中文路径下 fopen 为何失败，RSA 存密钥文件为何依赖它

### 阶段二：密文的运输箱（~30 分钟）
- [ ] 3. `common/base64.hpp` / `base64.cpp`
      - 重点：`Base64Encode`（cpp 25~58 行，3 字节→4 字符）；`Base64Decode`（87~117 行）
      - 了解 `Base64EncodeWrapped`：信封 Body 密文按 76 字符换行（MIME 惯例）
      - 验收：跑 `tests/test_base64.exe`，看懂 RFC 4648 官方向量

### 阶段三：OpenSSL 安全扶手（~20 分钟）
- [ ] 4. `crypto/openssl_util.hpp` / `openssl_util.cpp`
      - 重点：hpp 19~43 行 RAII 删除器（OpenSSL 裸指针谁释放？）；`RandomBytes`；错误队列要循环 `ERR_get_error()` 取完
      - 思考：为什么不能用 `rand()` 生成密钥/IV

### 阶段四：两个核心算法（各 1 小时）
- [ ] 5. `crypto/aes.hpp` → `aes.cpp` —— AES-256-CBC
      - hpp 20~22 行三个常量；Encrypt 的 **Init→Update→Final** 三步（cpp 44/56/63 行）
      - Decrypt 106~110 行：填充校验失败 = 密钥错/密文被篡改
      - 验收：`make test-crypto` 看 `TestAesNistVector`（官方测试向量）
- [ ] 6. `crypto/rsa.hpp` → `rsa.cpp` —— RSA-2048 + 签名
      - 分清 `RsaKey`（密钥管理）/ `Rsa`（算法）两个类
      - 加密：OAEP + SHA-256（cpp 126/130 行）→ 两段式 Encrypt（135~148 行）
      - **签名与加密的密钥方向相反**：私钥签名、公钥验签（198~262 行）
      - 思考：为什么 RSA-2048 单次最多 190 字节（hpp 59 行）？为什么密钥块恒为 256 字节？
      - 验收：`TestRsaKeyGenAndRoundTrip` / `TestRsaSignVerify`

### 阶段五：数字信封（核心，2 小时）★
- [ ] 7. `crypto/envelope.hpp` → `envelope.cpp`
      - 先背 hpp 顶部 6~16 行的 Seal/Open 流程注释
      - `Seal`（cpp 13~52 行）4 步：生成会话密钥 → AES 加密正文 → RSA 加密会话密钥 → 签名（**签的是密文**）
      - `Open`（54~93 行）3 步：反方向解包 + 验签
      - `Serialize`/`Parse`（97~193 行）：ASCII 信封拼装/解析
      - 注意：From/To 明文保留供路由；EncKey/IV/Signature/Body 全部加密或签名后 Base64
      - ✍️ **练手**：抄写副本 `practice/envelope_practice.cpp`（结构与步骤注释已给，实现挖空，照着本文件敲）
      - 验收：跑 `tests/demo_visual.exe` 看可视化演示

### 阶段六：用测试与应用反哺理解（1.5 小时）
- [ ] 8. `tests/test_crypto.cpp` —— 从 `main`（249~268 行）倒着读每个 `Test*`，说出每项在验证什么
- [ ] 9. `tests/demo_envelope.cpp` —— 按 `[1]密钥 → [2]Seal → [3]Serialize → [4]Parse+Open` 四步走一遍（`make demo`）
- [ ] 10. `tools/crypto_cli.cpp` / `tools/mail_crypto_gui.py` —— 会操作即可（`make cli`）

## 理解验收实验（挑 2~3 个做）

理解 ≠ 看懂，改坏它看它怎么报错才是真懂：
- [ ] 实验 A：把 `Aes::Encrypt` 的 key 换成 16 字节 → 观察 33 行长度校验拦截
- [ ] 实验 B：`Seal` 时不传 `sender_private_key`，`Open` 时传公钥 → 报"缺少数字签名"
- [ ] 实验 C：Parse 后把 `ciphertext[10] ^= 0xFF` 再 Open → 判断失败发生在 AES 层还是验签层
- [ ] 实验 D：换一把接收方私钥 Open → RSA OAEP 解码失败（"密钥不配对"）
- [ ] 实验 E：同一明文 Seal 两次 → 观察 IV 不同 → 密文不同（随机 IV 的直观证明）

## 时间安排建议

| 阶段 | 内容 | 预计 |
|---|---|---|
| 第 1 天 | 1~4 工具层 | 1~1.5 小时 |
| 第 2 天 | 5 AES | 1 小时 |
| 第 3 天 | 6 RSA | 1~1.5 小时 |
| 第 4~5 天 | 7 数字信封 ★ | 2 小时 |
| 第 6 天 | 8~10 测试与演示 | 1.5 小时 |

## 配套概念速查

- **对称加密**：加解密同一把钥匙（AES），快
- **非对称加密**：公钥/私钥两把（RSA），慢，用于传递对称密钥
- **IV（初始化向量）**：CBC 模式引入随机性，防相同明文产生相同密文
- **PKCS#7 填充**：CBC 要求明文是块（16B）整数倍，不足补满
- **OAEP**：RSA 的概率性填充方案（比 PKCS#1 v1.5 安全）
- **数字签名**：私钥对"内容摘要"加密，公钥验证 —— 保证完整性 + 认证身份
- **PEM**：密钥的文本存储格式（`-----BEGIN ...-----`）
