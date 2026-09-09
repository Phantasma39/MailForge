// ============================================================================
//  MailCrypto.h —— 邮件加密模块（MailForge 收发链路的加密统一入口）
//
//  ★ 本模块的两条加密通道均为【纯自研对称加密】，不依赖 OpenSSL：
//      1) ALGO_AES_CBC  —— AES-256-CBC（分组 + PKCS#7），见 crypto/aes.*
//      2) ALGO_CHACHA20 —— ChaCha20（RFC 8439 流密码），见 crypto/chacha20.*
//
//  密钥管理：每个账号一把 32 字节随机对称密钥文件，保存在 keyDir（默认
//  ./keys）下： keys/<用户名>.key
//    发信时用【收件人】的密钥加密，收信时用【当前查看账号】的密钥解密；
//    密钥文件不存在时自动生成（首次使用即自动补建，无需手工准备）。
//
//  密文线格式（正文区，可直接放 SMTP DATA / .eml）：
//    AES  : "MailForge::ENC::AES::" + Base64(随机16字节IV ‖ 密文)
//    ChaCha: "MailForge::ENC::CHA::" + Base64(随机12字节nonce ‖ 密文)
//    解密端按魔数头部自动识别算法（见 detectAlgo / decodeMail）。
// ============================================================================
#ifndef MAIL_CRYPTO_H
#define MAIL_CRYPTO_H

#include <string>

namespace MailCrypto {

// 支持的加密算法
enum CryptoAlgo {
    ALGO_NONE = 0,       // 不加密（默认，明文直传）
    ALGO_AES_CBC = 3,    // ★ AES-256-CBC 对称加密（自研）
    ALGO_CHACHA20 = 5    // ★ ChaCha20 流密码（自研，RFC 8439）
};

// 密文“魔数头”，用于识别一段文本是哪种算法加密过的
extern const char* kEncMagicAes;    // "MailForge::ENC::AES::"
extern const char* kEncMagicCha;    // "MailForge::ENC::CHA::"

// ---- 对外主接口 ----
// 加密整段邮件载荷。algo=ALGO_NONE 时原样返回。
// key 为对称密钥（AES/ChaCha 需要 32 字节，通常来自 getUserKey）。
// 返回：加密结果字符串（带魔数头 + Base64）。算法非法时按明文返回。
std::string encryptPayload(const std::string& plainText,
                           const std::string& key,
                           CryptoAlgo algo = ALGO_NONE);

// 解密(可能)加密过的文本。
//   cipherText 无魔数头 → 按原文返回（out=原文，返回 true）；
//   AES/CHA 头         → 用 key 解密；密钥错/密文被篡改/格式非法返回 false。
bool decryptPayload(const std::string& cipherText,
                    const std::string& key,
                    std::string& plainOut);

// 识别一段文本由哪种算法加密（未加密返回 ALGO_NONE）
CryptoAlgo detectAlgo(const std::string& text);

// 判断一段文本是不是本模块加密过的
bool isEncryptedText(const std::string& text);

// ---- 对称密钥文件管理 ----
// 确保某账号的 32 字节对称密钥存在（不存在则自动生成）并读出来。
// userKey 传邮箱地址或用户名均可（bob@example.com → bob）。
bool getUserKey(const std::string& userKey,
                std::string& keyOut,
                const std::string& keyDir = "./keys");

// 只确保密钥文件存在（不返回内容），供登录时预生成使用。
bool ensureUserKey(const std::string& userKey,
                   const std::string& keyDir = "./keys");

// ============================================================================
//  Web ↔ 服务器 RSA 信封（第一层：浏览器到 HTTP 服务器，调用 OpenSSL 库）
// ============================================================================
// 用途：本机没有 TLS 时，给浏览器↔8080 之间的邮件内容提供应用层机密性；
//       配合上文自研 AES/ChaCha（服务器 8080↔SMTP/POP3 端口之间、不调库），
//       构成 MailForge 的两层加密。
// 信封线格式（浏览器 WebCrypto 与 OpenSSL 互通，均为标准算法）：
//     k=<Base64 RSA-OAEP-SHA256(随机 AES-256-GCM 会话密钥)>|
//     iv=<Base64 GCM IV(12B)>|
//     ct=<Base64 AES-256-GCM 密文(末尾含 16B tag)>
// 浏览器端实现：WebCrypto（window.crypto.subtle，无第三方库）。
//
// 服务器 RSA-2048 密钥对（OpenSSL PEM，首次使用自动生成）：
//     <keyDir>/server_public.pem   发给浏览器做 RSA-OAEP（PKCS#8 SubjectPublicKeyInfo）
//     <keyDir>/server_private.pem  服务器解浏览器信封用

// 确保服务器 RSA-2048 密钥对存在（不存在则自动生成）
bool ensureWebRsaKeys(const std::string& keyDir = "./keys");

// 读取服务器 RSA 公钥（PKCS#8 PEM），返回给浏览器 import
bool webServerPublicKeyPem(std::string& pemOut,
                           const std::string& keyDir = "./keys");

// 服务器解开浏览器发来的 RSA 信封 → 明文（用服务器私钥）
bool webEnvelopeOpen(const std::string& packed,
                     std::string& plainOut,
                     const std::string& keyDir = "./keys");

// 服务器用收信方(浏览器上传的 RSA 公钥 PEM)封装一段明文 → 信封
bool webEnvelopeSeal(const std::string& plain,
                     const std::string& recipientPublicPem,
                     std::string& packedOut);

// ---- 基础工具 ----
// Base64 编码 / 解码（RFC 4648；编码时每 76 字符换行以兼容 SMTP 行长限制）
std::string base64Encode(const std::string& data);
std::string base64Decode(const std::string& text);

} // namespace MailCrypto

#endif // MAIL_CRYPTO_H

