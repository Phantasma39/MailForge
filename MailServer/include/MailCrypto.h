// ============================================================================
//  MailCrypto.h —— 邮件加密模块（MailForge 收发链路的加密统一入口）
//
//  设计目标：
//    这是"端到端邮件加密"的统一入口。主通道是【数字信封】（AES-256-CBC +
//    RSA-2048 混合加密 + SHA-256 签名，算法实现来自合并进来的 crypto-project
//    子系统，见仓库 crypto/、common/ 的 mail:: 命名空间）。
//    旧版 XOR 对称通道保留，用于兼容历史上用 XOR 发出的邮件。
//
//  本模块内部协议：
//    1) 不加密时：原文原样返回；
//    2) XOR 加密时：输出 = "MailForge::ENC::XOR::" + Base64(异或后的密文)；
//       解密时先认"签名头"，签名匹配就解，不匹配就按原文返回（兼容老邮件）。
//    3) 数字信封加密时：输出是一段 ASCII 信封文本
//       （-----BEGIN MAIL ENVELOPE----- ... -----END MAIL ENVELOPE-----），
//       可直接放进 SMTP DATA 原样传输 / 落盘，收件方凭自己的私钥拆封。
//
//  使用示例（在 HTTP 层已接线，见 HttpServer.cpp 的 handleSend / decodeMail）：
//    std::string envText;
//    MailCrypto::encryptEnvelope(mailText, from, to, envText);   // 发送前
//    MailCrypto::decryptEnvelope(envText, viewerUser, plain);    // 收取后
// ============================================================================
#ifndef MAIL_CRYPTO_H
#define MAIL_CRYPTO_H

#include <string>

namespace MailCrypto {

// 支持的加密算法（扩展新算法时在这里加一个枚举值即可）
enum CryptoAlgo {
    ALGO_NONE = 0,     // 不加密（默认，明文直传）
    ALGO_XOR = 1,      // XOR 异或加密（演示/兼容历史邮件，已实现）
    ALGO_RC4 = 2,      // RC4 流密码（预留，未实现）
    ALGO_AES_CBC = 3,  // AES-256-CBC 对称加密（信封的组成算法之一，预留单独通道）
    ALGO_ENVELOPE = 4  // ★ 数字信封：AES-256-CBC + RSA-2048（推荐通道，已接入收发流程）
};

// 加密后的"签名头"，用来识别一段文本是不是本模块加密过的
extern const char* kEncMagicXor;   // "MailForge::ENC::XOR::"

// ---- 对外主接口 ----
// 对整封邮件文本加密。algo=ALGO_NONE 时原样返回（走明文通道）。
// 输入：plainText 明文（可含中文，按字节处理）；key 对称密钥；
//      algo 想用的算法。
// 返回：加密结果字符串。
// 【TODO】加新算法：在内部 switch 里补分支。
std::string encryptPayload(const std::string& plainText,
                           const std::string& key,
                           CryptoAlgo algo = ALGO_NONE);

// 对(可能)加密过的文本解密。
// 输入：cipherText 收到的文本；key 对称密钥。
// 返回：解密后的明文；如果没有签名头（说明本来就没加密）就原样返回。
std::string decryptPayload(const std::string& cipherText,
                           const std::string& key);

// ==================== 数字信封接口（推荐加密通道） ====================
// 算法：AES-256-CBC（加密正文）+ RSA-2048（加密 AES 会话密钥）+ SHA-256 签名，
//       实现位于 contrib/crypto-project 合并进来的 crypto/、common/（mail:: 命名空间）。
// 密钥管理：每个用户一对 RSA-2048 密钥，保存在 keyDir（默认 ./keys）下：
//   <keyDir>/<用户名>.key.pem  私钥
//   <keyDir>/<用户名>.pub.pem  公钥
// 用户名取邮箱 @ 之前部分并转小写（bob@example.com → bob），与 POP3 收件目录规范一致。
// 首次使用（POP3 登录 / 发信）时若密钥不存在会自动生成，无需手工准备。

// 确保某账号的 RSA 密钥对存在（不存在则自动生成）。userKey 传邮箱地址或用户名均可。
bool ensureUserKeyPair(const std::string& userKey,
                       const std::string& keyDir = "./keys");

// 数字信封加密（发送前调用）：
//   用收件人(to)公钥加密随机会话密钥、AES-256-CBC 加密 plainText、
//   发件人(from)私钥对密文签名；
//   输出 ASCII 信封文本（可原样放进 SMTP DATA，也能被服务器当普通文本落盘）。
bool encryptEnvelope(const std::string& plainText,
                     const std::string& from,
                     const std::string& to,
                     std::string& envelopeText,
                     const std::string& keyDir = "./keys");

// 数字信封解密（收取后调用）：
//   viewerKey 是"当前查看邮件的收件人"（用其私钥拆封）；
//   发送方公钥若在本地存在则自动验签（防篡改 + 身份认证）。
bool decryptEnvelope(const std::string& envelopeText,
                     const std::string& viewerKey,
                     std::string& plainText,
                     const std::string& keyDir = "./keys");

// 判断一段文本是否为 ASCII 数字信封（含 "-----BEGIN MAIL ENVELOPE-----" 标记）
bool isEnvelopeText(const std::string& text);

// ---- 基础算法（各自独立、可单独调用，方便单元测试） ----
// Base64 编码 / 解码（3 字节 → 4 字符，RFC 4648）
std::string base64Encode(const std::string& data);
std::string base64Decode(const std::string& text);

// XOR 逐字节异或（key 循环使用；返回与输入等长的字节串）
std::string xorCipher(const std::string& data, const std::string& key);

} // namespace MailCrypto

#endif // MAIL_CRYPTO_H
