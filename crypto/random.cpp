// random.cpp —— CSPRNG 实现（Linux /dev/urandom，Windows BCryptGenRandom）
#include "crypto/random.hpp"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <cstdio>
#endif

namespace mail {

bool RandomBytes(unsigned char* buf, std::size_t len) {
  if (buf == nullptr || len == 0) return true;

#ifdef _WIN32
  // NTSTATUS == 0 表示成功
  return BCryptGenRandom(nullptr, buf, static_cast<ULONG>(len),
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
  FILE* f = std::fopen("/dev/urandom", "rb");
  if (!f) return false;
  const std::size_t got = std::fread(buf, 1, len, f);
  std::fclose(f);
  return got == len;
#endif
}

}  // namespace mail
