// demo_envelope.cpp —— 数字信封完整流程演示
//
// 运行流程：
//   1. 生成 发送方/接收方 RSA-2048 密钥对并保存到 keys/ 目录
//   2. Seal：加密一段中文邮件正文（AES-256-CBC + RSA + 签名）
//   3. Serialize：输出 ASCII 数字信封（可显示、可保存、可进 SMTP）
//   4. Parse + Open：从信封文本还原出原文并验签
//
// 编译运行：
//   make demo
//   ./tests/demo_envelope
#include <cstdio>
#include <string>
#include <vector>

#include "common/logger.hpp"
#include "crypto/envelope.hpp"
#include "crypto/rsa.hpp"

int main() {
  mail::LogSetLevel(mail::LogLevel::kInfo);
  std::printf("========== 数字信封完整流程演示 ==========\n\n");

  // ---- 1. 生成密钥对 ----
  mail::RsaKey sender, recipient;
  if (!mail::RsaKey::Generate(sender) || !mail::RsaKey::Generate(recipient)) {
    std::printf("密钥生成失败!\n");
    return 1;
  }
  sender.SavePrivateKey("keys/sender_private.pem");
  sender.SavePublicKey("keys/sender_public.pem");
  recipient.SavePrivateKey("keys/recipient_private.pem");
  recipient.SavePublicKey("keys/recipient_public.pem");
  std::printf("[1] 密钥对已生成并保存到 keys/ 目录（RSA-2048）\n\n");

  // ---- 2. 加密邮件正文 ----
  const std::string body =
      "计算机网络课程设计\n"
      "这是一封使用数字信封加密的邮件。\n"
      "安全特性：AES-256-CBC + RSA-2048 + SHA-256 签名\n";
  const std::vector<unsigned char> plaintext(body.begin(), body.end());

  mail::Envelope env;
  if (!mail::DigitalEnvelope::Seal(plaintext, "alice@test.com",
                                   "bob@test.com", sender.get(),
                                   recipient.get(), env)) {
    std::printf("加密失败!\n");
    return 1;
  }
  std::printf("[2] 已加密 %zu 字节正文：\n", plaintext.size());
  std::printf("    会话密钥(已RSA加密): %zu 字节\n", env.encrypted_key.size());
  std::printf("    数字签名: %zu 字节\n", env.signature.size());
  std::printf("    密文: %zu 字节\n\n", env.ciphertext.size());

  // ---- 3. 序列化为 ASCII 信封 ----
  std::string envelope_text;
  mail::DigitalEnvelope::Serialize(env, envelope_text);
  std::printf("[3] ASCII 数字信封（Base64 编码，可进 SMTP 传输）：\n");
  std::printf("%s", envelope_text.c_str());
  std::printf("\n");

  // ---- 4. 解析并解密 ----
  mail::Envelope env2;
  if (!mail::DigitalEnvelope::Parse(envelope_text, env2)) {
    std::printf("信封解析失败!\n");
    return 1;
  }
  std::vector<unsigned char> opened;
  if (!mail::DigitalEnvelope::Open(env2, recipient.get(), sender.get(),
                                   opened)) {
    std::printf("解密失败!\n");
    return 1;
  }
  std::printf("[4] 解密还原的原文（验签通过）：\n%s\n",
              std::string(opened.begin(), opened.end()).c_str());
  std::printf("========== 演示结束 ==========\n");
  return 0;
}
