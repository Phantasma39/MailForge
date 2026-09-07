// aes.hpp —— AES-256-CBC 对称加密
//
// 算法参数：
//   密钥 32 字节（256 bit），IV 16 字节（128 bit）
//   填充方式：PKCS#7（OpenSSL 默认）
//   实现：OpenSSL 3.0 EVP 高级 API（EVP_aes_256_cbc）
//
// 注意：Encrypt/Decrypt 失败时返回 false 并写 ERROR 日志；
//       GenerateKey/GenerateIv 失败时抛出 std::runtime_error（仅熵耗尽等灾难性情况）。
#ifndef MAIL_CRYPTO_AES_HPP_
#define MAIL_CRYPTO_AES_HPP_

#include <cstddef>
#include <vector>

namespace mail {

class Aes {
 public:
  static constexpr std::size_t kKeySize = 32;    // 256 bit
  static constexpr std::size_t kIvSize = 16;     // 128 bit
  static constexpr std::size_t kBlockSize = 16;  // CBC 块大小

  // 生成随机会话密钥 / IV（密码学安全随机源）
  static std::vector<unsigned char> GenerateKey();
  static std::vector<unsigned char> GenerateIv();

  // 加密：plaintext -> ciphertext（长度 = 明文 + 1~16 字节填充）
  static bool Encrypt(const std::vector<unsigned char>& key,
                      const std::vector<unsigned char>& iv,
                      const std::vector<unsigned char>& plaintext,
                      std::vector<unsigned char>& ciphertext);

  // 解密：ciphertext -> plaintext
  // 密钥/IV 错误或密文被篡改时因填充校验失败而返回 false
  static bool Decrypt(const std::vector<unsigned char>& key,
                      const std::vector<unsigned char>& iv,
                      const std::vector<unsigned char>& ciphertext,
                      std::vector<unsigned char>& plaintext);
};

}  // namespace mail

#endif  // MAIL_CRYPTO_AES_HPP_
