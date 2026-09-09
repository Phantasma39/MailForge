// openssl_util.cpp —— OpenSSL 3.0 工具层实现
#include "crypto/openssl_util.hpp"

#include <openssl/err.h>
#include <openssl/rand.h>

#include "common/logger.hpp"

namespace mail {

// 说明：mail::RandomBytes 由 crypto/random.cpp 提供（/dev/urandom 或
// BCryptGenRandom），这里不再重复定义，避免与随机源模块链接冲突。
// 本文件只保留 OpenSSL 错误队列工具（供 crypto/rsa.cpp 等打印错误）。

std::string OpenSslErrorString() {
  std::string result;
  unsigned long err = 0;
  while ((err = ERR_get_error()) != 0) {
    char buf[256];
    ERR_error_string_n(err, buf, sizeof(buf));
    if (!result.empty()) result += "; ";
    result += buf;
  }
  return result;
}

void LogOpenSslError(const std::string& context) {
  const std::string err = OpenSslErrorString();
  if (err.empty()) {
    LOG_ERROR(context << ": OpenSSL 操作失败");
  } else {
    LOG_ERROR(context << ": " << err);
  }
}

}  // namespace mail
