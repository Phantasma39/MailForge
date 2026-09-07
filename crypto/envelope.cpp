// envelope.cpp —— 数字信封实现
#include "crypto/envelope.hpp"

#include <sstream>

#include "common/base64.hpp"
#include "common/logger.hpp"
#include "crypto/aes.hpp"
#include "crypto/rsa.hpp"

namespace mail {

bool DigitalEnvelope::Seal(const std::vector<unsigned char>& plaintext,
                           const std::string& from,
                           const std::string& to,
                           EVP_PKEY* sender_private_key,
                           EVP_PKEY* recipient_public_key,
                           Envelope& out) {
  if (!recipient_public_key) {
    LOG_ERROR("信封 Seal: 接收方公钥为空");
    return false;
  }

  // 1) 随机生成 AES 会话密钥 + IV
  std::vector<unsigned char> session_key = Aes::GenerateKey();
  std::vector<unsigned char> iv = Aes::GenerateIv();

  // 2) AES-256-CBC 加密正文
  std::vector<unsigned char> ciphertext;
  if (!Aes::Encrypt(session_key, iv, plaintext, ciphertext)) return false;

  // 3) 用接收方公钥 RSA 加密会话密钥（数字信封的关键一步）
  std::vector<unsigned char> encrypted_key;
  if (!Rsa::PublicEncrypt(recipient_public_key, session_key,
                          encrypted_key)) {
    return false;
  }

  // 4) 发送方签名（可选）：对密文签名，保证完整性与身份认证
  std::vector<unsigned char> signature;
  if (sender_private_key != nullptr) {
    if (!Rsa::Sign(sender_private_key, ciphertext, signature)) return false;
  }

  out.encrypted_key = std::move(encrypted_key);
  out.iv = std::move(iv);
  out.signature = std::move(signature);
  out.ciphertext = std::move(ciphertext);
  out.from = from;
  out.to = to;
  return true;
}

bool DigitalEnvelope::Open(const Envelope& envelope,
                           EVP_PKEY* recipient_private_key,
                           EVP_PKEY* sender_public_key,
                           std::vector<unsigned char>& plaintext) {
  if (!recipient_private_key) {
    LOG_ERROR("信封 Open: 接收方私钥为空");
    return false;
  }

  // 1) 用自己的私钥解开 AES 会话密钥
  std::vector<unsigned char> session_key;
  if (!Rsa::PrivateDecrypt(recipient_private_key, envelope.encrypted_key,
                           session_key)) {
    return false;
  }
  if (session_key.size() != kSessionKeySize) {
    LOG_ERROR("信封 Open: 会话密钥长度异常 " << session_key.size());
    return false;
  }

  // 2) 用会话密钥 + IV 解密正文
  if (!Aes::Decrypt(session_key, envelope.iv, envelope.ciphertext,
                    plaintext)) {
    return false;
  }

  // 3) 用发送方公钥验签（可选）
  if (sender_public_key != nullptr) {
    if (envelope.signature.empty()) {
      LOG_ERROR("信封 Open: 缺少数字签名");
      return false;
    }
    if (!Rsa::Verify(sender_public_key, envelope.ciphertext,
                     envelope.signature)) {
      LOG_ERROR("信封 Open: 签名验证失败（内容被篡改或发送方不符）");
      return false;
    }
  }
  return true;
}

// ------------------------- 序列化 / 解析 -------------------------

bool DigitalEnvelope::Serialize(const Envelope& envelope, std::string& out) {
  if (envelope.encrypted_key.empty() || envelope.iv.empty() ||
      envelope.ciphertext.empty()) {
    LOG_ERROR("信封序列化: 信封数据不完整");
    return false;
  }
  out.clear();
  out += "-----BEGIN MAIL ENVELOPE-----\n";
  out += "Version: 1.0\n";
  out += "Cipher: AES-256-CBC\n";
  out += "From: " + envelope.from + "\n";
  out += "To: " + envelope.to + "\n";
  out += "EncKey: " + Base64Encode(envelope.encrypted_key) + "\n";
  out += "IV: " + Base64Encode(envelope.iv) + "\n";
  if (!envelope.signature.empty()) {
    out += "Signature: " + Base64Encode(envelope.signature) + "\n";
  }
  // Body 密文较长，按 MIME 惯例 76 字符换行
  out += "Body: " +
         Base64EncodeWrapped(envelope.ciphertext.data(),
                             envelope.ciphertext.size(), 76) +
         "\n";
  out += "-----END MAIL ENVELOPE-----\n";
  return true;
}

bool DigitalEnvelope::Parse(const std::string& text, Envelope& out) {
  out = Envelope();  // 重置输出
  std::istringstream iss(text);
  std::string line;
  bool in_envelope = false;
  bool in_body = false;
  std::string body_b64;

  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();

    if (line == "-----BEGIN MAIL ENVELOPE-----") {
      in_envelope = true;
      continue;
    }
    if (!in_envelope) continue;
    if (line == "-----END MAIL ENVELOPE-----") break;
    if (line.empty()) continue;

    // Body 字段为多行 Base64，后续行直接追加
    if (in_body) {
      body_b64 += line;
      continue;
    }

    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) return false;  // 格式错误
    const std::string key = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    const std::size_t first = value.find_first_not_of(" \t");
    value = (first == std::string::npos) ? std::string() : value.substr(first);

    if (key == "Body") {
      body_b64 = value;
      in_body = true;
    } else if (key == "From") {
      out.from = value;
    } else if (key == "To") {
      out.to = value;
    } else if (key == "EncKey") {
      auto bytes = Base64Decode(value);
      if (bytes.empty()) return false;
      out.encrypted_key = std::move(bytes);
    } else if (key == "IV") {
      auto bytes = Base64Decode(value);
      if (bytes.empty()) return false;
      out.iv = std::move(bytes);
    } else if (key == "Signature") {
      auto bytes = Base64Decode(value);
      if (bytes.empty()) return false;
      out.signature = std::move(bytes);
    }
    // Version / Cipher 等字段忽略（向后兼容）
  }

  if (!in_envelope) {
    LOG_ERROR("信封解析: 缺少起始标记");
    return false;
  }
  if (out.encrypted_key.empty() || out.iv.empty() || body_b64.empty()) {
    LOG_ERROR("信封解析: 必填字段缺失");
    return false;
  }
  auto body = Base64Decode(body_b64);
  if (body.empty()) {
    LOG_ERROR("信封解析: Body Base64 解码失败");
    return false;
  }
  out.ciphertext = std::move(body);
  return true;
}

}  // namespace mail
