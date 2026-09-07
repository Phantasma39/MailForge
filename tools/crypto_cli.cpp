// crypto_cli.cpp —— 加密模块命令行接口（供 GUI/脚本调用）
//
// 子命令（参数格式固定，便于外部程序解析）：
//   crypto_cli genkeys [keydir]
//       生成发送方/接收方 RSA-2048 密钥对，保存到 keydir（默认 keys/）
//   crypto_cli seal <from> <to> [keydir]
//       从 stdin 读取明文(UTF-8)，输出 ASCII 数字信封到 stdout
//   crypto_cli open [keydir]
//       从 stdin 读取信封文本，解密后输出明文到 stdout
//   crypto_cli tamper [keydir]
//       从 stdin 读取信封，篡改密文第 1 字节后尝试解密，报告检测结果
//   crypto_cli benchmark
//       输出性能基准测试结果
//
// 退出码：0=成功；1=业务/解密失败；2=参数错误
// 编码：stdin/stdout 一律 UTF-8 二进制字节（Windows 下关闭文本模式转换）
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <chrono>

#include "common/logger.hpp"
#include "crypto/aes.hpp"
#include "crypto/envelope.hpp"
#include "crypto/rsa.hpp"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

std::string ReadAllStdin() {
  std::ostringstream ss;
  ss << std::cin.rdbuf();
  return ss.str();
}

void WriteStdout(const std::string& s) {
  std::fwrite(s.data(), 1, s.size(), stdout);
  std::fflush(stdout);
}

int DoGenKeys(const std::string& dir) {
  mail::RsaKey sender, recipient;
  if (!mail::RsaKey::Generate(sender) || !mail::RsaKey::Generate(recipient)) {
    WriteStdout("ERROR: 密钥生成失败\n");
    return 1;
  }
  if (!sender.SavePrivateKey(dir + "/sender_private.pem") ||
      !sender.SavePublicKey(dir + "/sender_public.pem") ||
      !recipient.SavePrivateKey(dir + "/recipient_private.pem") ||
      !recipient.SavePublicKey(dir + "/recipient_public.pem")) {
    WriteStdout("ERROR: 密钥保存失败\n");
    return 1;
  }
  WriteStdout("OK: RSA-2048 密钥对已生成到 " + dir + "/\n");
  return 0;
}

int DoSeal(const std::string& from, const std::string& to,
           const std::string& dir) {
  mail::RsaKey sender, recipient;
  if (!mail::RsaKey::LoadPrivateKey(dir + "/sender_private.pem", sender) ||
      !mail::RsaKey::LoadPublicKey(dir + "/recipient_public.pem", recipient)) {
    WriteStdout("ERROR: 密钥加载失败，请先点击 [生成密钥]\n");
    return 1;
  }
  const std::string plaintext = ReadAllStdin();
  const std::vector<unsigned char> pt(plaintext.begin(), plaintext.end());
  mail::Envelope env;
  if (!mail::DigitalEnvelope::Seal(pt, from, to, sender.get(),
                                   recipient.get(), env)) {
    WriteStdout("ERROR: 加密失败\n");
    return 1;
  }
  std::string envelope_text;
  if (!mail::DigitalEnvelope::Serialize(env, envelope_text)) {
    WriteStdout("ERROR: 信封序列化失败\n");
    return 1;
  }
  WriteStdout(envelope_text);
  return 0;
}

int DoOpen(const std::string& dir) {
  mail::RsaKey recipient, sender;
  if (!mail::RsaKey::LoadPrivateKey(dir + "/recipient_private.pem", recipient) ||
      !mail::RsaKey::LoadPublicKey(dir + "/sender_public.pem", sender)) {
    WriteStdout("ERROR: 密钥加载失败，请先点击 [生成密钥]\n");
    return 1;
  }
  const std::string envelope_text = ReadAllStdin();
  mail::Envelope env;
  if (!mail::DigitalEnvelope::Parse(envelope_text, env)) {
    WriteStdout("ERROR: 信封解析失败\n");
    return 1;
  }
  std::vector<unsigned char> plaintext;
  if (!mail::DigitalEnvelope::Open(env, recipient.get(), sender.get(),
                                   plaintext)) {
    WriteStdout("ERROR: 解密失败（密钥不匹配或数据被篡改）\n");
    return 1;
  }
  WriteStdout(std::string(plaintext.begin(), plaintext.end()));
  return 0;
}

int DoTamper(const std::string& dir) {
  mail::RsaKey recipient, sender;
  if (!mail::RsaKey::LoadPrivateKey(dir + "/recipient_private.pem", recipient) ||
      !mail::RsaKey::LoadPublicKey(dir + "/sender_public.pem", sender)) {
    WriteStdout("ERROR: 密钥加载失败，请先点击 [生成密钥]\n");
    return 1;
  }
  const std::string envelope_text = ReadAllStdin();
  mail::Envelope env;
  if (!mail::DigitalEnvelope::Parse(envelope_text, env)) {
    WriteStdout("ERROR: 信封解析失败\n");
    return 1;
  }
  mail::Envelope tampered = env;
  tampered.ciphertext[0] ^= 0x01;  // 翻转密文第 1 字节模拟中间人篡改
  std::vector<unsigned char> plaintext;
  const bool ok = mail::DigitalEnvelope::Open(tampered, recipient.get(),
                                              sender.get(), plaintext);
  if (ok) {
    WriteStdout("WARN: 篡改后竟然解密成功（安全机制异常！）\n");
    return 1;
  }
  WriteStdout("OK: 篡改密文第 1 字节后解密失败 —— 内容完整性检测已生效\n");
  return 0;
}

int DoBenchmark() {
  std::vector<unsigned char> payload(1024 * 1024, 'M');
  std::vector<unsigned char> key = mail::Aes::GenerateKey();
  std::vector<unsigned char> iv = mail::Aes::GenerateIv();
  std::vector<unsigned char> ct, rt;
  auto t0 = std::chrono::steady_clock::now();
  mail::Aes::Encrypt(key, iv, payload, ct);
  auto t1 = std::chrono::steady_clock::now();
  mail::Aes::Decrypt(key, iv, ct, rt);
  auto t2 = std::chrono::steady_clock::now();
  const double aes_enc =
      std::chrono::duration<double, std::milli>(t1 - t0).count();
  const double aes_dec =
      std::chrono::duration<double, std::milli>(t2 - t1).count();

  mail::RsaKey rsa;
  mail::RsaKey::Generate(rsa);
  std::vector<unsigned char> sk = mail::Aes::GenerateKey();
  std::vector<unsigned char> rct, rrt;
  t1 = std::chrono::steady_clock::now();
  mail::Rsa::PublicEncrypt(rsa.get(), sk, rct);
  t2 = std::chrono::steady_clock::now();
  mail::Rsa::PrivateDecrypt(rsa.get(), rct, rrt);
  auto t3 = std::chrono::steady_clock::now();
  const double rsa_enc =
      std::chrono::duration<double, std::milli>(t2 - t1).count();
  const double rsa_dec =
      std::chrono::duration<double, std::milli>(t3 - t2).count();

  std::vector<unsigned char> sig;
  t2 = std::chrono::steady_clock::now();
  mail::Rsa::Sign(rsa.get(), payload, sig);
  t3 = std::chrono::steady_clock::now();
  mail::Rsa::Verify(rsa.get(), payload, sig);
  auto t4 = std::chrono::steady_clock::now();
  const double sign =
      std::chrono::duration<double, std::milli>(t3 - t2).count();
  const double verify =
      std::chrono::duration<double, std::milli>(t4 - t3).count();

  const double total = aes_enc + aes_dec + rsa_enc + rsa_dec + sign + verify;
  char buf[1024];
  std::snprintf(buf, sizeof(buf),
      "AES-256-CBC 加密1MB: %.2f ms\n"
      "AES-256-CBC 解密1MB: %.2f ms\n"
      "RSA-2048   公钥加密: %.2f ms\n"
      "RSA-2048   私钥解密: %.2f ms\n"
      "RSA-2048   签名1MB : %.2f ms\n"
      "RSA-2048   验签1MB : %.2f ms\n"
      "--------------------------------\n"
      "单封1MB邮件加密+签名+解密+验签: %.2f ms\n"
      "任务书指标 <2000ms → %s\n",
      aes_enc, aes_dec, rsa_enc, rsa_dec, sign, verify, total,
      (total < 2000.0) ? "达标" : "超标");
  WriteStdout(buf);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  // 关闭 stdin/stdout 的文本模式转换，保证 UTF-8 字节原样传输
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  mail::LogSetLevel(mail::LogLevel::kError);

  if (argc < 2) {
    WriteStdout("用法: crypto_cli <genkeys|seal|open|tamper|benchmark> ...\n");
    return 2;
  }
  const std::string cmd = argv[1];

  if (cmd == "genkeys") {
    const std::string dir = (argc >= 3) ? argv[2] : "keys";
    return DoGenKeys(dir);
  }
  if (cmd == "seal") {
    if (argc < 4) {
      WriteStdout("ERROR: seal 需要 <from> <to> 参数\n");
      return 2;
    }
    const std::string dir = (argc >= 5) ? argv[4] : "keys";
    return DoSeal(argv[2], argv[3], dir);
  }
  if (cmd == "open") {
    const std::string dir = (argc >= 3) ? argv[2] : "keys";
    return DoOpen(dir);
  }
  if (cmd == "tamper") {
    const std::string dir = (argc >= 3) ? argv[2] : "keys";
    return DoTamper(dir);
  }
  if (cmd == "benchmark") {
    return DoBenchmark();
  }
  WriteStdout("ERROR: 未知子命令 " + cmd + "\n");
  return 2;
}
