// aes.cpp —— AES-256-CBC 对称加密实现（OpenSSL 3.0 EVP）
#include "crypto/aes.hpp"

#include <stdexcept>

#include <openssl/evp.h>

#include "common/logger.hpp"
#include "crypto/openssl_util.hpp"

namespace mail {

std::vector<unsigned char> Aes::GenerateKey() {
  std::vector<unsigned char> key(kKeySize);
  if (!RandomBytes(key.data(), key.size())) {
    throw std::runtime_error("AES 密钥生成失败: " + OpenSslErrorString());
  }
  return key;
}

std::vector<unsigned char> Aes::GenerateIv() {
  std::vector<unsigned char> iv(kIvSize);
  if (!RandomBytes(iv.data(), iv.size())) {
    throw std::runtime_error("AES IV 生成失败: " + OpenSslErrorString());
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

  EvpCipherCtxPtr ctx(EVP_CIPHER_CTX_new());
  if (!ctx) {
    LogOpenSslError("AES 加密: 创建上下文失败");
    return false;
  }
  if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_cbc(), nullptr,
                         key.data(), iv.data()) != 1) {
    LogOpenSslError("AES 加密: 初始化失败");
    return false;
  }

  // PKCS#7 填充默认开启：密文最多比明文多一个块
  ciphertext.resize(plaintext.size() + kBlockSize);
  int outlen1 = 0;
  int outlen2 = 0;

  if (!plaintext.empty()) {
    if (EVP_EncryptUpdate(ctx.get(), ciphertext.data(), &outlen1,
                          plaintext.data(),
                          static_cast<int>(plaintext.size())) != 1) {
      LogOpenSslError("AES 加密: Update 失败");
      return false;
    }
  }
  if (EVP_EncryptFinal_ex(ctx.get(), ciphertext.data() + outlen1,
                          &outlen2) != 1) {
    LogOpenSslError("AES 加密: Final 失败");
    return false;
  }
  ciphertext.resize(static_cast<std::size_t>(outlen1 + outlen2));
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
  if (ciphertext.empty() || ciphertext.size() % kBlockSize != 0) {
    LOG_ERROR("AES 解密失败: 密文长度非法 " << ciphertext.size());
    return false;
  }

  EvpCipherCtxPtr ctx(EVP_CIPHER_CTX_new());
  if (!ctx) {
    LogOpenSslError("AES 解密: 创建上下文失败");
    return false;
  }
  if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_cbc(), nullptr,
                         key.data(), iv.data()) != 1) {
    LogOpenSslError("AES 解密: 初始化失败");
    return false;
  }

  plaintext.resize(ciphertext.size());
  int outlen1 = 0;
  int outlen2 = 0;
  if (EVP_DecryptUpdate(ctx.get(), plaintext.data(), &outlen1,
                        ciphertext.data(),
                        static_cast<int>(ciphertext.size())) != 1) {
    LogOpenSslError("AES 解密: Update 失败");
    return false;
  }
  if (EVP_DecryptFinal_ex(ctx.get(), plaintext.data() + outlen1,
                          &outlen2) != 1) {
    // 常见原因：密钥/IV 错误，或密文被篡改（PKCS#7 填充校验失败）
    LogOpenSslError("AES 解密: Final 失败（密钥错误或密文被篡改）");
    return false;
  }
  plaintext.resize(static_cast<std::size_t>(outlen1 + outlen2));
  return true;
}

}  // namespace mail
