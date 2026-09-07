// envelope.hpp —— 数字信封（Digital Envelope）混合加密
//
// ★ 本项目加密核心，对应任务书"提升要求"：
//   不少于 2 种加密算法协同（AES-256-CBC + RSA-2048 + SHA-256 签名，共 3 种）
//
// 加密（发送方，Seal）：
//   1) 随机生成 AES-256 会话密钥 + IV
//   2) AES-256-CBC 加密邮件正文 → ciphertext
//   3) 用【接收方公钥】RSA-OAEP 加密会话密钥 → encrypted_key
//   4) 用【发送方私钥】对密文做 SHA-256 签名 → signature（防篡改+身份认证）
//
// 解密（接收方，Open）：
//   1) 用【自己的私钥】解开 encrypted_key → 会话密钥
//   2) 用会话密钥 + IV 解密 ciphertext → 正文
//   3) 用【发送方公钥】验证 signature
//
// ASCII 序列化格式（SMTP 只能传输文本，密文经 Base64 编码）：
//   -----BEGIN MAIL ENVELOPE-----
//   Version: 1.0
//   Cipher: AES-256-CBC
//   From: alice@test.com
//   To: bob@test.com
//   EncKey: <Base64(RSA加密后的会话密钥)>
//   IV: <Base64(AES的IV)>
//   Signature: <Base64(签名)>
//   Body: <Base64(AES密文)，76字符自动换行>
//   -----END MAIL ENVELOPE-----
#ifndef MAIL_CRYPTO_ENVELOPE_HPP_
#define MAIL_CRYPTO_ENVELOPE_HPP_

#include <string>
#include <vector>

#include <openssl/evp.h>

namespace mail {

// 数字信封内容（密文数据 + 明文路由头）
struct Envelope {
  std::vector<unsigned char> encrypted_key;  // RSA 加密后的 AES 会话密钥
  std::vector<unsigned char> iv;             // AES 初始化向量（128 位）
  std::vector<unsigned char> signature;      // 发送方签名（可为空=未签名）
  std::vector<unsigned char> ciphertext;     // AES 加密后的正文密文
  std::string from;                          // 发送方邮箱（明文，供路由）
  std::string to;                            // 接收方邮箱（明文，供路由）
};

class DigitalEnvelope {
 public:
  // 加密封装（Seal）
  //   sender_private_key    发送方私钥（用于签名；传 nullptr 则不签名）
  //   recipient_public_key  接收方公钥（用于加密会话密钥）
  static bool Seal(const std::vector<unsigned char>& plaintext,
                   const std::string& from,
                   const std::string& to,
                   EVP_PKEY* sender_private_key,
                   EVP_PKEY* recipient_public_key,
                   Envelope& out);

  // 解密拆封（Open）
  //   recipient_private_key 接收方私钥（用于解密会话密钥）
  //   sender_public_key     发送方公钥（用于验签；传 nullptr 则不验签）
  static bool Open(const Envelope& envelope,
                   EVP_PKEY* recipient_private_key,
                   EVP_PKEY* sender_public_key,
                   std::vector<unsigned char>& plaintext);

  // 序列化为 ASCII 信封文本（可直接进 SMTP DATA）
  static bool Serialize(const Envelope& envelope, std::string& out);

  // 从 ASCII 信封文本解析恢复
  static bool Parse(const std::string& text, Envelope& out);

 private:
  static constexpr std::size_t kSessionKeySize = 32;  // AES-256
  static constexpr std::size_t kIvSize = 16;          // AES 块大小
};

}  // namespace mail

#endif  // MAIL_CRYPTO_ENVELOPE_HPP_
