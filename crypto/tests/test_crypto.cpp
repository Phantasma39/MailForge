// test_crypto.cpp —— 加密模块单元测试（自研 AES-256-CBC 与 ChaCha20）
//
// 覆盖：
//   1. AES-256-CBC：NIST SP 800-38A F.2.3 官方向量 + 随机往返 + 错误密钥检测
//   2. ChaCha20：RFC 8439 §2.3.2 官方向量 + 多块往返 + nonce 不同密钥流不同
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "common/logger.hpp"
#include "crypto/aes.hpp"
#include "crypto/chacha20.hpp"

namespace {

int g_failures = 0;

void Check(bool cond, const char* what) {
  if (!cond) {
    std::printf("[FAIL] %s\n", what);
    ++g_failures;
  } else {
    std::printf("[ OK ] %s\n", what);
  }
}

std::vector<unsigned char> FromHex(const std::string& hex) {
  auto nib = [](char c) -> unsigned char {
    if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned char>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<unsigned char>(c - 'A' + 10);
    return 0;
  };
  std::vector<unsigned char> out;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<unsigned char>((nib(hex[i]) << 4) | nib(hex[i + 1])));
  }
  return out;
}

// ---------------------------------------------------------------------------
// AES 测试
// ---------------------------------------------------------------------------

void TestAesNistVector() {
  // NIST SP 800-38A F.2.3 AES-256-CBC 第一分组测试向量
  const auto key = FromHex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4");
  const auto iv = FromHex("000102030405060708090a0b0c0d0e0f");
  const auto pt = FromHex("6bc1bee22e409f96e93d7e117393172a");
  const auto expected_ct = FromHex("f58c4c04d6e5f1ba779eabfb5f7bfbd6");

  std::vector<unsigned char> ct;
  Check(mail::Aes::Encrypt(key, iv, pt, ct), "AES: 加密成功");
  // PKCS7 填充：16 字节明文 -> 32 字节密文，前 16 字节应等于 NIST 向量
  Check(ct.size() == 32, "AES: 密文长度 = 明文 + 1 块（PKCS7）");
  const std::vector<unsigned char> first16(ct.begin(), ct.begin() + 16);
  Check(first16 == expected_ct, "AES: NIST SP800-38A 密文匹配");

  std::vector<unsigned char> rt;
  Check(mail::Aes::Decrypt(key, iv, ct, rt), "AES: 解密成功");
  Check(rt == pt, "AES: 解密还原明文");
}

void TestAesRandomRoundTrip() {
  std::mt19937 rng(20260902);
  const auto key = mail::Aes::GenerateKey();
  const auto iv = mail::Aes::GenerateIv();
  for (std::size_t len : {0u, 1u, 15u, 16u, 17u, 31u, 32u, 100u, 4096u,
                          1024u * 1024u}) {
    std::vector<unsigned char> pt(len);
    for (auto& b : pt) b = static_cast<unsigned char>(rng() & 0xFF);
    std::vector<unsigned char> ct, rt;
    Check(mail::Aes::Encrypt(key, iv, pt, ct), "AES随机: 加密");
    Check(mail::Aes::Decrypt(key, iv, ct, rt), "AES随机: 解密");
    Check(rt == pt, "AES随机: 往返一致");
  }
}

void TestAesWrongKey() {
  const auto key = mail::Aes::GenerateKey();
  const auto wrong_key = mail::Aes::GenerateKey();
  const auto iv = mail::Aes::GenerateIv();
  std::vector<unsigned char> pt(64, 'A');
  std::vector<unsigned char> ct, rt;
  Check(mail::Aes::Encrypt(key, iv, pt, ct), "AES错误密钥: 加密");
  const bool dec_ok = mail::Aes::Decrypt(wrong_key, iv, ct, rt);
  // 错误密钥解密：PKCS7 填充校验几乎必然失败；即使成功内容也必然错误
  Check(!dec_ok || rt != pt, "AES错误密钥: 解密失败或明文错误");
}

// ---------------------------------------------------------------------------
// ChaCha20 测试
// ---------------------------------------------------------------------------

void TestChaChaRfcVectors() {
  // RFC 8439 §2.3.2：key = 00..1f，nonce = 000000090000004a00000000
  std::vector<unsigned char> key(32);
  for (int i = 0; i < 32; ++i) key[i] = static_cast<unsigned char>(i);
  const auto nonce = FromHex("000000090000004a00000000");
  const auto expect1 = FromHex(
      "10f1e7e4d13b5915500fdd1fa32071c4"
      "c7d1f4c733c068030422aa9ac3d46c4e"
      "d2826446079faa0914c2d705d98b02a2"
      "b5129cd1de164eb9cbd083e8a2503c4e");

  unsigned char ks[mail::ChaCha20::kBlockSize];
  mail::ChaCha20::Block(key, nonce, 1, ks);
  Check(std::vector<unsigned char>(ks, ks + mail::ChaCha20::kBlockSize) == expect1,
        "ChaCha: RFC8439 Block#0(counter=1) 官方向量匹配");
}

void TestChaChaRoundTrip() {
  std::mt19937 rng(20260903);
  std::vector<unsigned char> key(32);
  for (auto& b : key) b = static_cast<unsigned char>(rng() & 0xFF);
  std::vector<unsigned char> nonce(12);
  for (auto& b : nonce) b = static_cast<unsigned char>(rng() & 0xFF);

  // 覆盖 0 ~ 200 字节：横跨多个 64 字节块，验证计数器递增逻辑
  for (std::size_t len : {0u, 1u, 63u, 64u, 65u, 127u, 128u, 200u}) {
    std::vector<unsigned char> pt(len);
    for (auto& b : pt) b = static_cast<unsigned char>(rng() & 0xFF);
    std::vector<unsigned char> ct, rt;
    Check(mail::ChaCha20::Crypt(key, nonce, pt, ct), "ChaCha: Crypt");
    Check(ct.size() == len, "ChaCha: 长度不变");
    if (len > 0) Check(ct != pt, "ChaCha: 密文 ≠ 明文");
    Check(mail::ChaCha20::Crypt(key, nonce, ct, rt), "ChaCha: Crypt(解密方向)");
    Check(rt == pt, "ChaCha: 往返一致");
  }

  // 相同 key 不同 nonce → 密钥流不同（否则流密码将可被 XOR 还原）
  std::vector<unsigned char> nonce2 = nonce;
  nonce2[0] ^= 0x80;
  std::vector<unsigned char> msg(100, 0x55), c1, c2;
  Check(mail::ChaCha20::Crypt(key, nonce, msg, c1), "ChaCha: 密文1");
  Check(mail::ChaCha20::Crypt(key, nonce2, msg, c2), "ChaCha: 密文2");
  Check(c1 != c2, "ChaCha: 不同 nonce 密钥流不同");
}

}  // namespace

int main() {
  mail::LogSetLevel(mail::LogLevel::kError);   // 仅失败时显示日志
  std::printf("===== 加密模块单元测试（自研 AES-256-CBC + ChaCha20）=====\n");
  TestAesNistVector();
  TestAesRandomRoundTrip();
  TestAesWrongKey();
  TestChaChaRfcVectors();
  TestChaChaRoundTrip();

  if (g_failures == 0) {
    std::printf("===== 全部通过 =====\n");
    return 0;
  }
  std::printf("===== 失败 %d 项 =====\n", g_failures);
  return 1;
}
