// chacha20.cpp —— ChaCha20 流密码自研实现（RFC 8439）
#include "crypto/chacha20.hpp"

namespace mail {
namespace {

inline std::uint32_t Rotl(std::uint32_t v, int n) {
  return (v << n) | (v >> (32 - n));
}

inline std::uint32_t Load32LE(const unsigned char* p) {
  return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8) |
         ((std::uint32_t)p[2] << 16) | ((std::uint32_t)p[3] << 24);
}

inline void Store32LE(std::uint32_t v, unsigned char* p) {
  p[0] = (unsigned char)(v & 0xFF);
  p[1] = (unsigned char)((v >> 8) & 0xFF);
  p[2] = (unsigned char)((v >> 16) & 0xFF);
  p[3] = (unsigned char)((v >> 24) & 0xFF);
}

// 四分之一轮：对状态中 4 个指定下标做 4 组“加法→异或→循环移位”
inline void QuarterRound(std::uint32_t st[16], int a, int b, int c, int d) {
  st[a] += st[b]; st[d] ^= st[a]; st[d] = Rotl(st[d], 16);
  st[c] += st[d]; st[b] ^= st[c]; st[b] = Rotl(st[b], 12);
  st[a] += st[b]; st[d] ^= st[a]; st[d] = Rotl(st[d], 8);
  st[c] += st[d]; st[b] ^= st[c]; st[b] = Rotl(st[b], 7);
}

// RFC 8439 §2.3.2：constants "expand 32-byte k"
constexpr std::uint32_t kConst[4] = {0x61707865, 0x3320646e, 0x79622d32,
                                     0x6b206574};

}  // namespace

void ChaCha20::Block(const std::vector<unsigned char>& key,
                     const std::vector<unsigned char>& nonce,
                     std::uint32_t counter,
                     unsigned char* out) {
  std::uint32_t state[16];

  // 初始状态：4 常量 + 8 密钥字 + 1 计数器 + 3 nonce 字（均小端装入）
  state[0] = kConst[0]; state[1] = kConst[1];
  state[2] = kConst[2]; state[3] = kConst[3];
  for (int i = 0; i < 8; ++i) state[4 + i] = Load32LE(&key[4 * i]);
  state[12] = counter;
  state[13] = Load32LE(&nonce[0]);
  state[14] = Load32LE(&nonce[4]);
  state[15] = Load32LE(&nonce[8]);

  std::uint32_t work[16];
  for (int i = 0; i < 16; ++i) work[i] = state[i];

  // 20 轮 = 10 次双轮（列轮 + 对角轮）
  for (int round = 0; round < 10; ++round) {
    // 列轮
    QuarterRound(work, 0, 4, 8, 12);
    QuarterRound(work, 1, 5, 9, 13);
    QuarterRound(work, 2, 6, 10, 14);
    QuarterRound(work, 3, 7, 11, 15);
    // 对角轮
    QuarterRound(work, 0, 5, 10, 15);
    QuarterRound(work, 1, 6, 11, 12);
    QuarterRound(work, 2, 7, 8, 13);
    QuarterRound(work, 3, 4, 9, 14);
  }

  // 与初始状态相加后小端输出 64 字节
  for (int i = 0; i < 16; ++i) {
    Store32LE(work[i] + state[i], out + 4 * i);
  }
}

bool ChaCha20::Crypt(const std::vector<unsigned char>& key,
                     const std::vector<unsigned char>& nonce,
                     const std::vector<unsigned char>& input,
                     std::vector<unsigned char>& output) {
  if (key.size() != kKeySize || nonce.size() != kNonceSize) return false;

  output.resize(input.size());
  unsigned char ks[kBlockSize];

  std::size_t pos = 0;
  std::uint32_t counter = 0;
  while (pos < input.size()) {
    Block(key, nonce, counter, ks);
    const std::size_t take = (input.size() - pos < kBlockSize)
                                 ? (input.size() - pos)
                                 : kBlockSize;
    for (std::size_t i = 0; i < take; ++i) {
      output[pos + i] = (unsigned char)(input[pos + i] ^ ks[i]);
    }
    pos += take;
    ++counter;   // 同一 nonce 下最多 2^32 个块（约 256 GiB），邮件远用不到
  }
  return true;
}

}  // namespace mail
