// test_crypto.cpp —— 加密模块单元测试
//
// 覆盖：
//   1. AES-256-CBC：NIST SP 800-38A 官方向量 + 随机往返 + 错误密钥检测
//   2. RSA-2048：密钥生成、加解密往返、签名/验签、篡改检测
//   3. 数字信封：Seal/Open 往返、ASCII 序列化/解析、防篡改
//   4. 密钥管理：PEM 保存/加载后仍可正常加解密
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "common/logger.hpp"
#include "crypto/aes.hpp"
#include "crypto/envelope.hpp"
#include "crypto/rsa.hpp"

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
  // 错误密钥解密：PKCS7 填充校验大概率失败；即使成功内容也必然错误
  Check(!dec_ok || rt != pt, "AES错误密钥: 解密失败或明文错误");
}

// ---------------------------------------------------------------------------
// RSA 测试
// ---------------------------------------------------------------------------

void TestRsaKeyGenAndRoundTrip() {
  mail::RsaKey key;
  Check(mail::RsaKey::Generate(key), "RSA: 密钥生成");
  Check(key.valid(), "RSA: 密钥有效");
  Check(mail::Rsa::kMaxEncryptSize > 32, "RSA: 可加密 32 字节会话密钥");

  std::vector<unsigned char> data(32);
  for (int i = 0; i < 32; ++i) data[i] = static_cast<unsigned char>(i);
  std::vector<unsigned char> ct, rt;
  Check(mail::Rsa::PublicEncrypt(key.get(), data, ct), "RSA: 公钥加密");
  Check(ct.size() == 256, "RSA: 密文长度 = 256 字节");
  Check(mail::Rsa::PrivateDecrypt(key.get(), ct, rt), "RSA: 私钥解密");
  Check(rt == data, "RSA: 加解密往返一致");
}

void TestRsaSignVerify() {
  mail::RsaKey key;
  Check(mail::RsaKey::Generate(key), "RSA签名: 密钥生成");
  std::vector<unsigned char> data(1024, 'M');
  std::vector<unsigned char> sig;
  Check(mail::Rsa::Sign(key.get(), data, sig), "RSA签名: 签名");
  Check(sig.size() == 256, "RSA签名: 签名长度 = 256");
  Check(mail::Rsa::Verify(key.get(), data, sig), "RSA签名: 验签通过");

  data[0] = 'X';  // 篡改内容
  Check(!mail::Rsa::Verify(key.get(), data, sig), "RSA签名: 篡改后验签失败");
}

// ---------------------------------------------------------------------------
// 数字信封测试
// ---------------------------------------------------------------------------

void TestEnvelopeRoundTrip() {
  mail::RsaKey sender, recipient;
  Check(mail::RsaKey::Generate(sender), "信封: 发送方密钥生成");
  Check(mail::RsaKey::Generate(recipient), "信封: 接收方密钥生成");

  const std::string body = "你好，这是一封加密邮件的正文。\nHello, this is a secure mail.\n";
  const std::vector<unsigned char> pt(body.begin(), body.end());

  mail::Envelope env;
  Check(mail::DigitalEnvelope::Seal(pt, "alice@test.com", "bob@test.com",
                                    sender.get(), recipient.get(), env),
        "信封: Seal 加密");
  Check(env.encrypted_key.size() == 256, "信封: RSA 加密的会话密钥 = 256 字节");
  Check(env.iv.size() == 16, "信封: IV = 16 字节");
  Check(!env.signature.empty(), "信封: 已签名");

  std::vector<unsigned char> opened;
  Check(mail::DigitalEnvelope::Open(env, recipient.get(), sender.get(), opened),
        "信封: Open 解密");
  Check(opened == pt, "信封: 加解密往返一致");
}

void TestEnvelopeSerialize() {
  mail::RsaKey sender, recipient;
  Check(mail::RsaKey::Generate(sender), "信封序列化: 发送方密钥");
  Check(mail::RsaKey::Generate(recipient), "信封序列化: 接收方密钥");

  const std::string body = "Serialize test body 序列化测试，包含中文内容。";
  const std::vector<unsigned char> pt(body.begin(), body.end());
  mail::Envelope env;
  Check(mail::DigitalEnvelope::Seal(pt, "a@test.com", "b@test.com",
                                    sender.get(), recipient.get(), env),
        "信封序列化: Seal");

  std::string text;
  Check(mail::DigitalEnvelope::Serialize(env, text), "信封序列化: Serialize");
  Check(text.rfind("-----BEGIN MAIL ENVELOPE-----", 0) == 0,
        "信封序列化: 起始标记正确");
  Check(text.find("EncKey:") != std::string::npos,
        "信封序列化: 含 EncKey 字段");
  Check(text.find("Signature:") != std::string::npos,
        "信封序列化: 含 Signature 字段");

  mail::Envelope env2;
  Check(mail::DigitalEnvelope::Parse(text, env2), "信封序列化: Parse");
  Check(env2.encrypted_key == env.encrypted_key, "信封序列化: EncKey 一致");
  Check(env2.iv == env.iv, "信封序列化: IV 一致");
  Check(env2.signature == env.signature, "信封序列化: 签名一致");
  Check(env2.ciphertext == env.ciphertext, "信封序列化: 密文一致");
  Check(env2.from == "a@test.com" && env2.to == "b@test.com",
        "信封序列化: 头部一致");

  std::vector<unsigned char> opened;
  Check(mail::DigitalEnvelope::Open(env2, recipient.get(), sender.get(), opened),
        "信封序列化: 解析后仍可解密");
  Check(opened == pt, "信封序列化: 解密正文一致");
}

void TestEnvelopeTamper() {
  mail::RsaKey sender, recipient, other;
  Check(mail::RsaKey::Generate(sender), "信封防篡改: 发送方密钥");
  Check(mail::RsaKey::Generate(recipient), "信封防篡改: 接收方密钥");
  Check(mail::RsaKey::Generate(other), "信封防篡改: 第三方密钥");

  const std::string body = "tamper test 防篡改测试";
  const std::vector<unsigned char> pt(body.begin(), body.end());
  mail::Envelope env;
  Check(mail::DigitalEnvelope::Seal(pt, "a@test.com", "b@test.com",
                                    sender.get(), recipient.get(), env),
        "信封防篡改: Seal");
  std::vector<unsigned char> opened;

  // 篡改密文：解密或验签应失败
  mail::Envelope bad1 = env;
  bad1.ciphertext[10] ^= 0xFF;
  Check(!mail::DigitalEnvelope::Open(bad1, recipient.get(), sender.get(), opened),
        "信封防篡改: 密文篡改后 Open 失败");

  // 篡改签名：验签应失败
  mail::Envelope bad2 = env;
  bad2.signature[0] ^= 0xFF;
  Check(!mail::DigitalEnvelope::Open(bad2, recipient.get(), sender.get(), opened),
        "信封防篡改: 签名篡改后验签失败");

  // 错误接收方解密：OAEP 解码失败
  Check(!mail::DigitalEnvelope::Open(env, other.get(), sender.get(), opened),
        "信封防篡改: 非接收方解密失败");
}

void TestKeyFiles() {
  mail::RsaKey key;
  Check(mail::RsaKey::Generate(key), "密钥文件: 生成");

  const std::string priv = "keys/test_priv.pem";
  const std::string pub = "keys/test_pub.pem";
  Check(key.SavePrivateKey(priv), "密钥文件: 保存私钥");
  Check(key.SavePublicKey(pub), "密钥文件: 保存公钥");

  mail::RsaKey loaded_priv, loaded_pub;
  Check(mail::RsaKey::LoadPrivateKey(priv, loaded_priv), "密钥文件: 加载私钥");
  Check(mail::RsaKey::LoadPublicKey(pub, loaded_pub), "密钥文件: 加载公钥");

  // 加载后的密钥仍可正常加解密
  std::vector<unsigned char> data(32, 0x77);
  std::vector<unsigned char> ct, rt;
  Check(mail::Rsa::PublicEncrypt(loaded_pub.get(), data, ct),
        "密钥文件: 加载的公钥可加密");
  Check(mail::Rsa::PrivateDecrypt(loaded_priv.get(), ct, rt),
        "密钥文件: 加载的私钥可解密");
  Check(rt == data, "密钥文件: 加载后加解密往返一致");

  // 清理测试文件
  std::remove(priv.c_str());
  std::remove(pub.c_str());
}

}  // namespace

int main() {
  mail::LogSetLevel(mail::LogLevel::kError);  // 仅失败时显示日志
  std::printf("===== 加密模块单元测试 =====\n");
  TestAesNistVector();
  TestAesRandomRoundTrip();
  TestAesWrongKey();
  TestRsaKeyGenAndRoundTrip();
  TestRsaSignVerify();
  TestEnvelopeRoundTrip();
  TestEnvelopeSerialize();
  TestEnvelopeTamper();
  TestKeyFiles();

  if (g_failures == 0) {
    std::printf("===== 全部通过 =====\n");
    return 0;
  }
  std::printf("===== 失败 %d 项 =====\n", g_failures);
  return 1;
}

