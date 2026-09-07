// aes.cpp —— AES-256-CBC 对称加密自研实现（不依赖任何加密库）
//
// 自研内容：
//   - AES-256 密钥扩展（32 字节密钥 → 15 个轮密钥，共 240 字节）
//   - 加密轮：SubBytes / ShiftRows / MixColumns / AddRoundKey
//   - 解密轮：InvSubBytes / InvShiftRows / InvMixColumns / AddRoundKey
//   - CBC 链接模式 + PKCS#7 填充
//   - S 盒按规范用 GF(2^8) 求逆 + 仿射变换生成（不是抄表），逆 S 盒由映射反推
// 安全性与正确性：同一密钥每封邮件必须用不同的随机 IV；官方向量
//   NIST SP 800-38A F.2.3 在 crypto/tests/test_crypto.cpp 中回归。
#include "crypto/aes.hpp"

#include <array>
#include <cstring>
#include <stdexcept>

#include "common/logger.hpp"
#include "crypto/random.hpp"

namespace mail {

namespace {

constexpr std::size_t kBlock = 16;     // 128 bit
constexpr int kRounds = 14;            // AES-256 轮数
constexpr std::size_t kExpandedBytes = kBlock * (kRounds + 1);  // 240

// ---------- GF(2^8) 基础运算（不可约多项式 x^8+x^4+x^3+x+1，即 0x11B） ----------

inline unsigned char Xtime(unsigned char x) {
  return (unsigned char)((x << 1) ^ ((x & 0x80) ? 0x1B : 0x00));
}

unsigned char GfMul(unsigned char a, unsigned char b) {
  unsigned char r = 0;
  while (b) {
    if (b & 1) r ^= a;
    a = Xtime(a);
    b >>= 1;
  }
  return r;
}

// GF(2^8) 求逆：非零元 a^(254) == a^(-1)（该域乘法群的阶为 255）
unsigned char GfInv(unsigned char a) {
  if (a == 0) return 0;
  unsigned char r = 1;
  for (int i = 0; i < 254; ++i) r = GfMul(r, a);
  return r;
}

// AES 仿射变换：b -> A·b ^ 0x63
// 展开式等价于 b ^ rol(b,1) ^ rol(b,2) ^ rol(b,3) ^ rol(b,4) ^ 0x63
unsigned char Affine(unsigned char b) {
  b = (unsigned char)(b ^ ((b << 1) | (b >> 7)) ^ ((b << 2) | (b >> 6)) ^
                      ((b << 3) | (b >> 5)) ^ ((b << 4) | (b >> 4)));
  return (unsigned char)(b ^ 0x63);
}

// 构建 S 盒与逆 S 盒（首次调用时生成，C++11 起静态初始化线程安全）
const std::array<unsigned char, 256>& SBox() {
  static const std::array<unsigned char, 256> box = [] {
    std::array<unsigned char, 256> sb{};
    for (int x = 1; x < 256; ++x) {
      sb[(std::size_t)x] = Affine(GfInv((unsigned char)x));
    }
    sb[0] = 0x63;
    return sb;
  }();
  return box;
}

const std::array<unsigned char, 256>& InvSBox() {
  static const std::array<unsigned char, 256> isb = [] {
    std::array<unsigned char, 256> t{};
    for (int x = 0; x < 256; ++x) t[(std::size_t)SBox()[(std::size_t)x]] =
        (unsigned char)x;
    return t;
  }();
  return isb;
}

inline std::uint32_t Load32BE(const unsigned char* p) {
  return ((std::uint32_t)p[0] << 24) | ((std::uint32_t)p[1] << 16) |
         ((std::uint32_t)p[2] << 8) | (std::uint32_t)p[3];
}

inline void Store32BE(std::uint32_t v, unsigned char* p) {
  p[0] = (unsigned char)(v >> 24);
  p[1] = (unsigned char)((v >> 16) & 0xFF);
  p[2] = (unsigned char)((v >> 8) & 0xFF);
  p[3] = (unsigned char)(v & 0xFF);
}

// ---------- AES-256 密钥扩展 ----------

void ExpandKey(const unsigned char key[32], unsigned char rk[kExpandedBytes]) {
  std::array<std::uint32_t, 60> w{};   // 4*(Nr+1) = 60 个字
  for (int i = 0; i < 8; ++i) w[(std::size_t)i] = Load32BE(key + 4 * i);

  // 轮常数 Rcon[i] = x^(i-1)（用于 i 是 8 的倍数的字）
  auto rcon = [](int i) {
    unsigned char v = 1;
    for (int k = 1; k < i; ++k) v = Xtime(v);
    return v;
  };

  for (int i = 8; i < 60; ++i) {
    std::uint32_t t = w[(std::size_t)(i - 1)];
    if (i % 8 == 0) {
      // RotWord -> SubWord -> XOR Rcon
      const std::uint32_t rotated = (t << 8) | (t >> 24);
      t = rotated;
      unsigned char b[4];
      Store32BE(t, b);
      b[0] = SBox()[b[0]]; b[1] = SBox()[b[1]];
      b[2] = SBox()[b[2]]; b[3] = SBox()[b[3]];
      t = Load32BE(b);
      t ^= (std::uint32_t)rcon(i / 8) << 24;
    } else if (i % 8 == 4) {
      // AES-256 特有的中间 SubWord
      unsigned char b[4];
      Store32BE(t, b);
      b[0] = SBox()[b[0]]; b[1] = SBox()[b[1]];
      b[2] = SBox()[b[2]]; b[3] = SBox()[b[3]];
      t = Load32BE(b);
    }
    w[(std::size_t)i] = w[(std::size_t)(i - 8)] ^ t;
  }

  // 展开成 15 个连续 16 字节轮密钥
  for (int i = 0; i < 60; ++i) Store32BE(w[(std::size_t)i], rk + 4 * i);
}

// ---------- 字节状态变换（状态按列优先排布：s[col*4 + row]） ----------

void AddRoundKey(unsigned char s[16], const unsigned char* rk) {
  for (int i = 0; i < 16; ++i) s[i] ^= rk[i];
}

void SubBytes(unsigned char s[16]) {
  for (int i = 0; i < 16; ++i) s[i] = SBox()[s[i]];
}

void InvSubBytes(unsigned char s[16]) {
  for (int i = 0; i < 16; ++i) s[i] = InvSBox()[s[i]];
}

void ShiftRows(unsigned char s[16]) {
  unsigned char t[16];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) t[c * 4 + r] = s[((c + r) & 3) * 4 + r];
  std::memcpy(s, t, 16);
}

void InvShiftRows(unsigned char s[16]) {
  unsigned char t[16];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) t[c * 4 + r] = s[((c - r + 4) & 3) * 4 + r];
  std::memcpy(s, t, 16);
}

inline unsigned char Mul2(unsigned char x) { return Xtime(x); }
inline unsigned char Mul3(unsigned char x) { return (unsigned char)(Xtime(x) ^ x); }
inline unsigned char Mul9(unsigned char x) {
  return (unsigned char)(Xtime(Xtime(Xtime(x))) ^ x);
}
inline unsigned char Mul11(unsigned char x) {
  return (unsigned char)(Xtime(Xtime(Xtime(x))) ^ Xtime(x) ^ x);
}
inline unsigned char Mul13(unsigned char x) {
  return (unsigned char)(Xtime(Xtime(Xtime(x))) ^ Xtime(Xtime(x)) ^ x);
}
inline unsigned char Mul14(unsigned char x) {
  return (unsigned char)(Xtime(Xtime(Xtime(x))) ^ Xtime(Xtime(x)) ^ Xtime(x));
}

void MixColumns(unsigned char s[16]) {
  unsigned char t[16];
  for (int c = 0; c < 4; ++c) {
    unsigned char a0 = s[c * 4 + 0], a1 = s[c * 4 + 1];
    unsigned char a2 = s[c * 4 + 2], a3 = s[c * 4 + 3];
    t[c * 4 + 0] = (unsigned char)(Mul2(a0) ^ Mul3(a1) ^ a2 ^ a3);
    t[c * 4 + 1] = (unsigned char)(a0 ^ Mul2(a1) ^ Mul3(a2) ^ a3);
    t[c * 4 + 2] = (unsigned char)(a0 ^ a1 ^ Mul2(a2) ^ Mul3(a3));
    t[c * 4 + 3] = (unsigned char)(Mul3(a0) ^ a1 ^ a2 ^ Mul2(a3));
  }
  std::memcpy(s, t, 16);
}

void InvMixColumns(unsigned char s[16]) {
  unsigned char t[16];
  for (int c = 0; c < 4; ++c) {
    unsigned char a0 = s[c * 4 + 0], a1 = s[c * 4 + 1];
    unsigned char a2 = s[c * 4 + 2], a3 = s[c * 4 + 3];
    t[c * 4 + 0] =
        (unsigned char)(Mul14(a0) ^ Mul11(a1) ^ Mul13(a2) ^ Mul9(a3));
    t[c * 4 + 1] =
        (unsigned char)(Mul9(a0) ^ Mul14(a1) ^ Mul11(a2) ^ Mul13(a3));
    t[c * 4 + 2] =
        (unsigned char)(Mul13(a0) ^ Mul9(a1) ^ Mul14(a2) ^ Mul11(a3));
    t[c * 4 + 3] =
        (unsigned char)(Mul11(a0) ^ Mul13(a1) ^ Mul9(a2) ^ Mul14(a3));
  }
  std::memcpy(s, t, 16);
}

void EncryptBlock(const unsigned char rk[kExpandedBytes],
                  const unsigned char in[16], unsigned char out[16]) {
  unsigned char s[16];
  std::memcpy(s, in, 16);
  AddRoundKey(s, rk);   // 轮 0
  for (int r = 1; r < kRounds; ++r) {
    SubBytes(s);
    ShiftRows(s);
    MixColumns(s);
    AddRoundKey(s, rk + 16 * r);
  }
  SubBytes(s);
  ShiftRows(s);
  AddRoundKey(s, rk + 16 * kRounds);
  std::memcpy(out, s, 16);
}

void DecryptBlock(const unsigned char rk[kExpandedBytes],
                  const unsigned char in[16], unsigned char out[16]) {
  unsigned char s[16];
  std::memcpy(s, in, 16);
  AddRoundKey(s, rk + 16 * kRounds);
  for (int r = kRounds - 1; r >= 1; --r) {
    InvShiftRows(s);
    InvSubBytes(s);
    AddRoundKey(s, rk + 16 * r);
    InvMixColumns(s);
  }
  InvShiftRows(s);
  InvSubBytes(s);
  AddRoundKey(s, rk);   // 轮 0
  std::memcpy(out, s, 16);
}

}  // namespace

std::vector<unsigned char> Aes::GenerateKey() {
  std::vector<unsigned char> key(kKeySize);
  if (!RandomBytes(key.data(), key.size())) {
    throw std::runtime_error("AES 密钥生成失败（随机源不可用）");
  }
  return key;
}

std::vector<unsigned char> Aes::GenerateIv() {
  std::vector<unsigned char> iv(kIvSize);
  if (!RandomBytes(iv.data(), iv.size())) {
    throw std::runtime_error("AES IV 生成失败（随机源不可用）");
  }
  return iv;
}

bool Aes::Encrypt(const std::vector<unsigned char>& key,
                  const std::vector<unsigned char>& iv,
                  const std::vector<unsigned char>& plaintext,
                  std::vector<unsigned char>& ciphertext) {
  if (key.size() != kKeySize || iv.size() != kIvSize) {
    LOG_ERROR("AES 加密失败: 密钥长度 " << key.size()
              << " / IV 长度 " << iv.size());
    return false;
  }

  // PKCS#7：填充 1..16 字节，即使明文已是 16 的倍数也补整块
  const std::size_t pad = kBlock - (plaintext.size() % kBlock);
  std::vector<unsigned char> buf(plaintext.size() + pad);
  if (!plaintext.empty())
    std::memcpy(buf.data(), plaintext.data(), plaintext.size());
  for (std::size_t i = 0; i < pad; ++i)
    buf[plaintext.size() + i] = (unsigned char)pad;

  std::array<unsigned char, kExpandedBytes> rk{};
  ExpandKey(key.data(), rk.data());

  ciphertext.resize(buf.size());
  unsigned char prev[kBlock];
  std::memcpy(prev, iv.data(), kBlock);

  for (std::size_t b = 0; b < buf.size(); b += kBlock) {
    unsigned char blk[kBlock];
    for (int i = 0; i < 16; ++i) blk[i] = (unsigned char)(buf[b + i] ^ prev[i]);
    EncryptBlock(rk.data(), blk, ciphertext.data() + b);
    std::memcpy(prev, ciphertext.data() + b, kBlock);
  }
  return true;
}

bool Aes::Decrypt(const std::vector<unsigned char>& key,
                  const std::vector<unsigned char>& iv,
                  const std::vector<unsigned char>& ciphertext,
                  std::vector<unsigned char>& plaintext) {
  if (key.size() != kKeySize || iv.size() != kIvSize) {
    LOG_ERROR("AES 解密失败: 密钥长度 " << key.size()
              << " / IV 长度 " << iv.size());
    return false;
  }
  if (ciphertext.empty() || ciphertext.size() % kBlock != 0) {
    LOG_ERROR("AES 解密失败: 密文长度非法 " << ciphertext.size());
    return false;
  }

  std::array<unsigned char, kExpandedBytes> rk{};
  ExpandKey(key.data(), rk.data());

  std::vector<unsigned char> out(ciphertext.size());
  unsigned char prev[kBlock];
  std::memcpy(prev, iv.data(), kBlock);

  for (std::size_t b = 0; b < ciphertext.size(); b += kBlock) {
    unsigned char blk[kBlock];
    DecryptBlock(rk.data(), ciphertext.data() + b, blk);
    for (int i = 0; i < 16; ++i) out[b + i] = (unsigned char)(blk[i] ^ prev[i]);
    std::memcpy(prev, ciphertext.data() + b, kBlock);
  }

  // PKCS#7 填充校验：密钥错误或密文被篡改时大概率在此失败
  const unsigned char p = out.back();
  if (p == 0 || p > kBlock) {
    LOG_ERROR("AES 解密失败: PKCS#7 填充非法（密钥错误或密文被篡改）");
    return false;
  }
  for (std::size_t i = 0; i < p; ++i) {
    if (out[out.size() - 1 - i] != p) {
      LOG_ERROR("AES 解密失败: PKCS#7 填充校验失败");
      return false;
    }
  }
  out.resize(out.size() - p);
  plaintext.swap(out);
  return true;
}

}  // namespace mail
