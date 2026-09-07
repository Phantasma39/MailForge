// rsa.hpp —— RSA-2048 非对称加密 + 数字签名
//
// 模块组成：
//   RsaKey   密钥对 RAII 封装：生成、PEM 保存/加载
//   Rsa      静态算法函数：公钥加密 / 私钥解密 / 签名 / 验签
//
// 安全参数（OpenSSL 3.0 EVP）：
//   加密填充：RSA-OAEP + SHA-256（比 PKCS#1 v1.5 更安全）
//   签名方案：SHA-256 摘要 + RSASSA-PKCS1-v1_5
//   RSA-2048 下 OAEP-SHA256 单次最多加密 190 字节（32 字节 AES 密钥绰绰有余）
#ifndef MAIL_CRYPTO_RSA_HPP_
#define MAIL_CRYPTO_RSA_HPP_

#include <string>
#include <vector>

#include <openssl/evp.h>

#include "crypto/openssl_util.hpp"

namespace mail {

// ===========================================================================
// RSA 密钥对（RAII 封装）
// ===========================================================================
class RsaKey {
 public:
  RsaKey() = default;
  ~RsaKey() = default;

  RsaKey(const RsaKey&) = delete;
  RsaKey& operator=(const RsaKey&) = delete;
  RsaKey(RsaKey&&) noexcept = default;
  RsaKey& operator=(RsaKey&&) noexcept = default;

  EVP_PKEY* get() const { return pkey_.get(); }
  bool valid() const { return pkey_ != nullptr; }

  // 生成新密钥对（默认 2048 位）
  static bool Generate(RsaKey& out, int bits = 2048);

  // PEM 格式保存 / 加载（路径支持 UTF-8 中文目录）
  bool SavePrivateKey(const std::string& path) const;
  bool SavePublicKey(const std::string& path) const;
  static bool LoadPrivateKey(const std::string& path, RsaKey& out);
  static bool LoadPublicKey(const std::string& path, RsaKey& out);

 private:
  EvpPkeyPtr pkey_;
};

// ===========================================================================
// RSA 算法函数
// ===========================================================================
class Rsa {
 public:
  static constexpr int kDefaultBits = 2048;
  // OAEP-SHA256 下 RSA-2048 单次可加密的最大明文长度 = 256 - 2*32 - 2
  static constexpr std::size_t kMaxEncryptSize = 190;

  // 公钥加密（RSA-OAEP, SHA-256），密文长度恒为 256 字节
  static bool PublicEncrypt(EVP_PKEY* pubkey,
                            const std::vector<unsigned char>& plaintext,
                            std::vector<unsigned char>& ciphertext);

  // 私钥解密（与 PublicEncrypt 配对）
  static bool PrivateDecrypt(EVP_PKEY* privkey,
                             const std::vector<unsigned char>& ciphertext,
                             std::vector<unsigned char>& plaintext);

  // 私钥签名：对数据做 SHA-256 摘要并 RSA 签名，签名长度恒为 256 字节
  static bool Sign(EVP_PKEY* privkey,
                   const std::vector<unsigned char>& data,
                   std::vector<unsigned char>& signature);

  // 公钥验签：成功返回 true；内容被篡改 / 公钥不匹配返回 false
  static bool Verify(EVP_PKEY* pubkey,
                     const std::vector<unsigned char>& data,
                     const std::vector<unsigned char>& signature);
};

}  // namespace mail

#endif  // MAIL_CRYPTO_RSA_HPP_
