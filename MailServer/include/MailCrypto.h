// ============================================================================
//  MailCrypto.h —— 邮件加密模块（预留接口）
//
//  设计目标：
//    这是"传输过程加密"的统一入口。目前完整实现了 NONE(不加密) 与 XOR 两种，
//    并在代码里标好了【TODO】位置——以后要加 AES-CBC / RC4 / SM4 等算法时，
//    只需在 encryptPayload() / decryptPayload() 里加一个分支即可，
//    HTTP 服务层不用改动，做到了"加密算法可插拔"。
//
//  约定（本模块内部协议）：
//    1) 不加密时：原文原样返回；
//    2) XOR 加密时：输出 = "MailForge::ENC::XOR::" + Base64(异或后的密文)。
//       解密时先认"签名头"，签名匹配就解，不匹配就按原文返回（兼容没加密的老邮件）。
//    3) 为什么套一层 Base64：密文可能是任意二进制，直接塞进 SMTP 的文本协议
//       会被换行/编码问题搞坏；Base64 把所有字节变成可见 ASCII，才能安全传输。
//
//  使用示例（在 HTTP 层已接线）：
//    std::string cipher = MailCrypto::encryptPayload(plainMail, key, ALGO_XOR);
//    std::string plain  = MailCrypto::decryptPayload(cipher,  key);
// ============================================================================
#ifndef MAIL_CRYPTO_H
#define MAIL_CRYPTO_H

#include <string>

namespace MailCrypto {

// 支持的加密算法（扩展新算法时在这里加一个枚举值即可）
enum CryptoAlgo {
    ALGO_NONE = 0,   // 不加密（默认，明文直传）
    ALGO_XOR = 1,    // XOR 异或加密（演示用，已实现）
    ALGO_RC4 = 2,    // RC4 流密码（【TODO】后续实现）
    ALGO_AES_CBC = 3 // AES-256-CBC（【TODO】后续实现，可基于 OpenSSL 等）
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

// ---- 基础算法（各自独立、可单独调用，方便单元测试） ----
// Base64 编码 / 解码（3 字节 → 4 字符，RFC 4648）
std::string base64Encode(const std::string& data);
std::string base64Decode(const std::string& text);

// XOR 逐字节异或（key 循环使用；返回与输入等长的字节串）
std::string xorCipher(const std::string& data, const std::string& key);

} // namespace MailCrypto

#endif // MAIL_CRYPTO_H
