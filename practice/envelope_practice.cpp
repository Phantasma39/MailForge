// envelope_practice.cpp —— 数字信封【练手空白副本】
//
// 用法：
//   1) 打开 crypto/envelope.cpp（标准答案）与 本文件 左右并排
//   2) 从 Seal 开始，逐个函数把实现敲进本文件（先别看答案，卡住再看）
//   3) 全部敲完后告诉我，我帮你编译 + 用测试验证对错
//
// 提示：函数签名与头文件 crypto/envelope.hpp 完全一致，不能改。
//       每步该做什么我写在函数体注释里了，你负责"翻译"成 C++。
#include <sstream>
#include <string>
#include <vector>

#include "common/base64.hpp"
#include "common/logger.hpp"
#include "crypto/aes.hpp"
#include "crypto/envelope.hpp"
#include "crypto/rsa.hpp"

namespace mail {

// ===========================================================================
// 1) Seal —— 加密封装（发送方视角）
//    目标：明文 -> Envelope{EncKey, IV, Signature, Ciphertext, From, To}
// ===========================================================================
bool DigitalEnvelope::Seal(const std::vector<unsigned char>& plaintext,
                           const std::string& from,
                           const std::string& to,
                           EVP_PKEY* sender_private_key,
                           EVP_PKEY* recipient_public_key,
                           Envelope& out) {
  // [1] 防御：接收方公钥为空 → LOG_ERROR 后 return false
  // TODO(student): 补全
if (!recipient_public_key){
  LOG_ERROR("信封 Seal: 接收方公钥为空");
    return false;
}
  // [2] 生成随机会话密钥 + IV（Aes::GenerateKey / Aes::GenerateIv）
  // TODO(student): 补全
  std::vector<unsigned char> session_key = Aes::GenerateKey();
  std::vector<unsigned char> iv = Aes::GenerateIv();
  // [3] AES-256-CBC 加密正文 → ciphertext（失败直接 return false）
  // TODO(student): 补全
 std::vector<unsigned char> ciphertext;
  if (!Aes::Encrypt(session_key, iv, plaintext, ciphertext)) return false;

  // [4] 用【接收方公钥】RSA-OAEP 加密会话密钥 → encrypted_key
  //     （数字信封最关键的一步，失败 return false）
  // TODO(student): 补全

  // [5] 若 sender_private_key 非空：对 ciphertext 签名 → signature
  //     ★ 签的是密文不是明文！失败 return false
  // TODO(student): 补全

  // [6] 把 5 个字段 + from/to 装进 out（可用 std::move 转移）
  // TODO(student): 补全

  return true;
}

// ===========================================================================
// 2) Open —— 解密拆封（接收方视角）
//    目标：Envelope + 私钥 -> 还原 plaintext（并验签）
// ===========================================================================
bool DigitalEnvelope::Open(const Envelope& envelope,
                           EVP_PKEY* recipient_private_key,
                           EVP_PKEY* sender_public_key,
                           std::vector<unsigned char>& plaintext) {
  // [1] 防御：接收方私钥为空 → LOG_ERROR 后 return false
  // TODO(student): 补全

  // [2] 用自己的私钥解密 envelope.encrypted_key → session_key
  //     （失败 = 密钥不配对/被篡改，return false）
  // TODO(student): 补全

  // [3] 校验 session_key 长度 == DigitalEnvelope::kSessionKeySize（32）
  //     注意：kSessionKeySize 是私有静态成员，类内可直接用
  // TODO(student): 补全

  // [4] 用 session_key + envelope.iv 解密密文 → plaintext
  // TODO(student): 补全

  // [5] 若 sender_public_key 非空，则必须验签：
  //     - envelope.signature 为空 → LOG_ERROR 并 return false
  //     - Rsa::Verify(sender_public_key, ciphertext, signature) 失败 → return false
  // TODO(student): 补全

  return true;
}

// ===========================================================================
// 3) Serialize —— 把信封结构拼成 ASCII 文本（可直接进 SMTP DATA）
// ===========================================================================
bool DigitalEnvelope::Serialize(const Envelope& envelope, std::string& out) {
  // [1] 完整性检查：encrypted_key / iv / ciphertext 三者都不能为空
  // TODO(student): 补全

  // [2] 拼头部（注意每行以 \n 结尾）：
  //     -----BEGIN MAIL ENVELOPE-----
  //     Version: 1.0
  //     Cipher: AES-256-CBC
  //     From: <envelope.from>
  //     To:   <envelope.to>
  // TODO(student): 补全

  // [3] 二进制字段用 Base64 编码成一行：
  //     EncKey: Base64Encode(envelope.encrypted_key)
  //     IV:     Base64Encode(envelope.iv)
  //     （signature 非空时才输出 Signature 行）
  // TODO(student): 补全

  // [4] Body 密文较长，用 Base64EncodeWrapped(..., 76) 每 76 字符换行
  // TODO(student): 补全

  // [5] 收尾标记 -----END MAIL ENVELOPE-----
  // TODO(student): 补全

  return true;
}

// ===========================================================================
// 4) Parse —— 把 ASCII 信封文本解析回 Envelope 结构（Serialize 的逆过程）
// ===========================================================================
bool DigitalEnvelope::Parse(const std::string& text, Envelope& out) {
  // [1] out = Envelope() 重置；准备 istringstream + 逐行 getline
  //     （每行去掉结尾 \r，兼容 Windows 换行）
  // TODO(student): 补全

  // [2] 行状态机：
  //     - 遇到 "-----BEGIN MAIL ENVELOPE-----" → in_envelope = true
  //     - 遇到 "-----END MAIL ENVELOPE-----"   → 结束循环
  //     - Body 字段是多行的：Body 行之后的所有非空行都属于 body_b64
  //       （需要 in_body 标志）
  // TODO(student): 补全

  // [3] 其他字段按 "Key: value" 解析：
  //     - value 去掉前导空格
  //     - EncKey / IV / Signature 用 Base64Decode 解码（空结果=失败）
  //     - From / To 直接存字符串
  //     - Version / Cipher 等忽略即可
  // TODO(student): 补全

  // [4] 收尾校验：
  //     - 没见到 BEGIN 标记 → 失败
  //     - EncKey/IV/Body 任一为空 → 失败
  //     - 最后 Base64Decode(body_b64) → out.ciphertext
  // TODO(student): 补全

  return true;
}

}  // namespace mail
