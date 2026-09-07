// openssl_util.hpp —— OpenSSL 3.0 工具层
//
// 提供：
//   1) RAII 智能指针（自动释放 OpenSSL 对象，杜绝内存泄漏）
//   2) 密码学安全随机数生成
//   3) 错误队列读取与日志输出
#ifndef MAIL_CRYPTO_OPENSSL_UTIL_HPP_
#define MAIL_CRYPTO_OPENSSL_UTIL_HPP_

#include <cstddef>
#include <memory>
#include <string>

#include <openssl/evp.h>

namespace mail {

// ------------------------- RAII 删除器 -------------------------
struct EvpPkeyDeleter {
  void operator()(EVP_PKEY* p) const {
    if (p) EVP_PKEY_free(p);
  }
};
struct EvpCipherCtxDeleter {
  void operator()(EVP_CIPHER_CTX* p) const {
    if (p) EVP_CIPHER_CTX_free(p);
  }
};
struct EvpPkeyCtxDeleter {
  void operator()(EVP_PKEY_CTX* p) const {
    if (p) EVP_PKEY_CTX_free(p);
  }
};
struct EvpMdCtxDeleter {
  void operator()(EVP_MD_CTX* p) const {
    if (p) EVP_MD_CTX_free(p);
  }
};

using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;
using EvpCipherCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, EvpCipherCtxDeleter>;
using EvpPkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, EvpPkeyCtxDeleter>;
using EvpMdCtxPtr = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;

// ------------------------- 工具函数 -------------------------

// 生成密码学安全随机字节（基于 OpenSSL CSPRNG），失败返回 false
bool RandomBytes(unsigned char* buf, std::size_t len);

// 读取并清空 OpenSSL 错误队列，返回拼接的错误描述
std::string OpenSslErrorString();

// 将当前错误队列以 ERROR 级别写入日志（带操作上下文说明）
void LogOpenSslError(const std::string& context);

}  // namespace mail

#endif  // MAIL_CRYPTO_OPENSSL_UTIL_HPP_
