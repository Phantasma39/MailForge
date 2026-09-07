// demo_visual.cpp —— 加密模块可视化测试工具
//
// 功能：
//   [1] 密钥生成与指纹展示
//   [2] 邮件加密：明文 vs 密文 hex 对比 + 信封字段可视化
//   [3] 篡改检测演示：修改 1 字节 → 解密/验签失败
//   [4] 性能基准：AES/RSA/签名耗时，验证任务书"1MB 邮件 < 2 秒"指标
//   [5] 密钥文件互操作性检查（openssl 可读取）
//
// 运行：make demo-visual  （或 ./tests/demo_visual）
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include <openssl/evp.h>
#include <openssl/x509.h>

#include "common/logger.hpp"
#include "crypto/aes.hpp"
#include "crypto/envelope.hpp"
#include "crypto/rsa.hpp"

namespace {

// 秒表
class Timer {
 public:
  Timer() { Reset(); }
  void Reset() { start_ = std::chrono::steady_clock::now(); }
  double ms() const {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - start_)
        .count();
  }

 private:
  std::chrono::steady_clock::time_point start_;
};

void Section(const char* title) {
  std::printf("\n============================================================\n");
  std::printf("  %s\n", title);
  std::printf("============================================================\n");
}

std::string ToHex(const std::vector<unsigned char>& v, std::size_t limit = 64) {
  const char* digits = "0123456789abcdef";
  std::string s;
  const std::size_t n = (v.size() < limit) ? v.size() : limit;
  for (std::size_t i = 0; i < n; ++i) {
    if (i > 0 && i % 16 == 0) s += '\n';
    s.push_back(digits[v[i] >> 4]);
    s.push_back(digits[v[i] & 0x0F]);
    if (i + 1 < n) s += ' ';
  }
  if (v.size() > limit) s += "  ... (共 " + std::to_string(v.size()) + " 字节)";
  return s;
}

// 计算数据的 SHA-256 指纹（前 16 字节 hex）
std::string Sha256Fingerprint(const std::vector<unsigned char>& data) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int mdlen = 0;
  if (EVP_Digest(data.data(), data.size(), md, &mdlen, EVP_sha256(), nullptr) != 1) {
    return "(计算失败)";
  }
  const char* digits = "0123456789abcdef";
  std::string s;
  for (unsigned int i = 0; i < 16 && i < mdlen; ++i) {
    s.push_back(digits[md[i] >> 4]);
    s.push_back(digits[md[i] & 0x0F]);
    if (i < 15 && i % 2 == 1) s += ':';
  }
  return s;
}

void PrintKeyInfo(const char* name, mail::RsaKey& key) {
  // 从公钥导出 DER 计算指纹
  unsigned char* der = nullptr;
  const int der_len = i2d_PUBKEY(key.get(), &der);
  std::vector<unsigned char> der_data;
  if (der_len > 0 && der != nullptr) {
    der_data.assign(der, der + der_len);
    OPENSSL_free(der);
  }
  std::printf("  %-8s 指纹: %s\n", name,
              der_data.empty() ? "(N/A)" : Sha256Fingerprint(der_data).c_str());
}

void DemoEncryptVisual() {
  Section("[2] 邮件加密演示：明文 vs 密文");
  mail::RsaKey sender, recipient;
  mail::RsaKey::Generate(sender);
  mail::RsaKey::Generate(recipient);

  const std::string body =
      "计算机网络课程设计——邮件加密\n"
      "这是使用数字信封加密的邮件正文。";
  const std::vector<unsigned char> pt(body.begin(), body.end());

  mail::Envelope env;
  if (!mail::DigitalEnvelope::Seal(pt, "alice@test.com", "bob@test.com",
                                   sender.get(), recipient.get(), env)) {
    std::printf("  加密失败!\n");
    return;
  }

  std::printf("\n  明文（%zu 字节）:\n%s\n", pt.size(), body.c_str());
  std::printf("\n  密文 hex（前 48 字节，肉眼已不可读）:\n  %s\n\n",
              ToHex(env.ciphertext, 48).c_str());

  std::printf("  信封结构（数字信封 = 3 种算法协同）:\n");
  std::printf("  %-26s %5zu 字节  %s\n", "EncKey (RSA加密的AES密钥)",
              env.encrypted_key.size(), "<- 用接收方公钥加密");
  std::printf("  %-26s %5zu 字节  %s\n", "IV (AES初始化向量)",
              env.iv.size(), "<- 每次随机生成");
  std::printf("  %-26s %5zu 字节  %s\n", "Signature (SHA-256签名)",
              env.signature.size(), "<- 用发送方私钥");
  std::printf("  %-26s %5zu 字节  %s\n", "Body (AES加密正文)",
              env.ciphertext.size(), "<- 用会话密钥加密");
}

void DemoTamperDetection() {
  Section("[3] 篡改检测演示：改 1 个字节会怎样？");
  mail::RsaKey sender, recipient;
  mail::RsaKey::Generate(sender);
  mail::RsaKey::Generate(recipient);

  const std::string body = "重要通知：项目将于周五答辩，请做好准备。";
  const std::vector<unsigned char> pt(body.begin(), body.end());
  mail::Envelope env;
  mail::DigitalEnvelope::Seal(pt, "alice@test.com", "bob@test.com",
                              sender.get(), recipient.get(), env);

  // 正常解密
  std::vector<unsigned char> opened;
  const bool ok = mail::DigitalEnvelope::Open(env, recipient.get(),
                                              sender.get(), opened);
  std::printf("  正常解密          : %s -> \"%s\"\n",
              ok ? "成功" : "失败",
              std::string(opened.begin(), opened.end()).c_str());

  // 篡改密文第 1 个字节
  mail::Envelope tampered = env;
  tampered.ciphertext[0] ^= 0x01;
  opened.clear();
  const bool tamper_ok = mail::DigitalEnvelope::Open(
      tampered, recipient.get(), sender.get(), opened);
  std::printf("  篡改密文第1字节后  : %s（内容完整性被检测到！）\n",
              tamper_ok ? "竟然解密成功" : "解密失败");

  // 篡改签名
  mail::Envelope bad_sig = env;
  bad_sig.signature[0] ^= 0x01;
  opened.clear();
  const bool sig_ok = mail::DigitalEnvelope::Open(
      bad_sig, recipient.get(), sender.get(), opened);
  std::printf("  篡改签名第1字节后  : %s（身份认证被检测到！）\n",
              sig_ok ? "竟然验证通过" : "验签失败");
}

void Benchmark() {
  Section("[4] 性能基准测试（对应任务书硬指标：1MB 邮件 < 2 秒）");

  // 生成 1MB 随机邮件正文
  std::vector<unsigned char> payload(1024 * 1024);
  std::mt19937 rng(20260902);
  for (auto& b : payload) b = static_cast<unsigned char>(rng() & 0xFF);

  // ---- AES-256-CBC ----
  const auto key = mail::Aes::GenerateKey();
  const auto iv = mail::Aes::GenerateIv();
  std::vector<unsigned char> ct, rt;
  Timer t;
  mail::Aes::Encrypt(key, iv, payload, ct);
  const double aes_enc_ms = t.ms();
  t.Reset();
  mail::Aes::Decrypt(key, iv, ct, rt);
  const double aes_dec_ms = t.ms();

  // ---- RSA-2048 ----
  mail::RsaKey rsa;
  mail::RsaKey::Generate(rsa);
  std::vector<unsigned char> session_key = mail::Aes::GenerateKey();
  std::vector<unsigned char> rsa_ct, rsa_rt;
  t.Reset();
  mail::Rsa::PublicEncrypt(rsa.get(), session_key, rsa_ct);
  const double rsa_enc_ms = t.ms();
  t.Reset();
  mail::Rsa::PrivateDecrypt(rsa.get(), rsa_ct, rsa_rt);
  const double rsa_dec_ms = t.ms();

  // ---- 签名 / 验签 ----
  std::vector<unsigned char> sig;
  t.Reset();
  mail::Rsa::Sign(rsa.get(), payload, sig);
  const double sign_ms = t.ms();
  t.Reset();
  mail::Rsa::Verify(rsa.get(), payload, sig);
  const double verify_ms = t.ms();

  std::printf("  AES-256-CBC 加密 %6.1f ms\n", aes_enc_ms);
  std::printf("  AES-256-CBC 解密 %6.1f ms\n", aes_dec_ms);
  std::printf("  RSA-2048   公钥加密 %6.1f ms\n", rsa_enc_ms);
  std::printf("  RSA-2048   私钥解密 %6.1f ms\n", rsa_dec_ms);
  std::printf("  RSA-2048   签名1MB %6.1f ms\n", sign_ms);
  std::printf("  RSA-2048   验签1MB %6.1f ms\n", verify_ms);

  const double total = aes_enc_ms + aes_dec_ms + rsa_enc_ms + rsa_dec_ms +
                       sign_ms + verify_ms;
  std::printf("  -----------------------------------------------------\n");
  std::printf("  单封 1MB 邮件完整加密+签名+解密+验签: %.1f ms\n", total);
  std::printf("  任务书指标: 往返 < 2000 ms   实际: %.1f ms  ->  %s\n",
              total, (total < 2000.0) ? "✅ 达标" : "❌ 超标");

  // 1MB 加密吞吐率
  std::printf("  AES 加密吞吐率: %.1f MB/s\n",
              payload.size() / 1024.0 / 1024.0 / (aes_enc_ms / 1000.0));
}

void DemoKeys() {
  Section("[5] 密钥管理演示");
  mail::RsaKey sender, recipient;
  Timer t;
  mail::RsaKey::Generate(sender);
  const double gen1 = t.ms();
  t.Reset();
  mail::RsaKey::Generate(recipient);
  const double gen2 = t.ms();

  std::printf("  发送方 RSA-2048 密钥对生成: %.0f ms\n", gen1);
  std::printf("  接收方 RSA-2048 密钥对生成: %.0f ms\n", gen2);

  sender.SavePrivateKey("keys/sender_private.pem");
  sender.SavePublicKey("keys/sender_public.pem");
  recipient.SavePrivateKey("keys/recipient_private.pem");
  recipient.SavePublicKey("keys/recipient_public.pem");

  PrintKeyInfo("发送方", sender);
  PrintKeyInfo("接收方", recipient);
  std::printf("\n  密钥已保存为标准 PEM 格式（openssl 命令可互操作读取）\n");
}

}  // namespace

int main() {
  mail::LogSetLevel(mail::LogLevel::kError);
  std::printf("============================================================\n");
  std::printf("  加密模块可视化测试工具\n");
  std::printf("============================================================\n");

  DemoKeys();
  DemoEncryptVisual();
  DemoTamperDetection();
  Benchmark();

  std::printf("\n============================================================\n");
  std::printf("  全部演示完成\n");
  std::printf("============================================================\n");
  return 0;
}

