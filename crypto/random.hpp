// random.hpp —— 密码学安全随机数（不依赖任何加密库）
//
// 用途：生成 AES 密钥/IV、ChaCha20 nonce、账号对称密钥文件。
//   实现：Linux/Unix 读 /dev/urandom；Windows 用 BCryptGenRandom。
#ifndef MAIL_CRYPTO_RANDOM_HPP_
#define MAIL_CRYPTO_RANDOM_HPP_

#include <cstddef>

namespace mail {

// 生成 len 字节密码学安全随机数。成功返回 true。
bool RandomBytes(unsigned char* buf, std::size_t len);

}  // namespace mail

#endif  // MAIL_CRYPTO_RANDOM_HPP_
