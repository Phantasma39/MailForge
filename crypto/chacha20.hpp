// chacha20.hpp —— ChaCha20 流密码（RFC 8439）自研实现
//
// 算法参数（RFC 8439 IETF 变体）：
//   密钥 32 字节（256 bit），nonce 12 字节，块计数器 32 位
//   核心运算：32 位加法 / 异或 / 循环移位（ARX），无查表、无第三方依赖
//   安全性：同一 (key, nonce) 绝不能重复使用，否则两个密文异或可还原明文。
// 官方验收向量：RFC 8439 §2.3.2（见 crypto/tests/test_crypto.cpp）
#ifndef MAIL_CRYPTO_CHACHA20_HPP_
#define MAIL_CRYPTO_CHACHA20_HPP_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mail {

class ChaCha20 {
 public:
  static constexpr std::size_t kKeySize = 32;    // 256 bit
  static constexpr std::size_t kNonceSize = 12;  // 96 bit
  static constexpr std::size_t kBlockSize = 64;  // 每个块的密钥流长度

  // 生成第 counter 个 64 字节密钥流块（RFC 8439 §2.3.2 的官方向量即以此接口验证）
  // key 必须 32 字节、nonce 必须 12 字节；out 指向至少 64 字节的缓冲区。
  static void Block(const std::vector<unsigned char>& key,
                    const std::vector<unsigned char>& nonce,
                    std::uint32_t counter,
                    unsigned char* out);

  // 加解密（流密码加解密是同一操作）：
  //   output = input XOR keystream(key, nonce)，counter 从 0 开始逐块递增
  //   成功返回 true；密钥/nonce 长度错误返回 false。
  static bool Crypt(const std::vector<unsigned char>& key,
                    const std::vector<unsigned char>& nonce,
                    const std::vector<unsigned char>& input,
                    std::vector<unsigned char>& output);
};

}  // namespace mail

#endif  // MAIL_CRYPTO_CHACHA20_HPP_
