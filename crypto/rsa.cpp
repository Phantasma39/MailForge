// rsa.cpp —— RSA-2048 非对称加密 + 数字签名实现（OpenSSL 3.0 EVP）
#include "crypto/rsa.hpp"

#include <openssl/pem.h>
#include <openssl/rsa.h>

#include "common/file_util.hpp"
#include "common/logger.hpp"

namespace mail {

// --------------------------- RsaKey ---------------------------

bool RsaKey::Generate(RsaKey& out, int bits) {
  EvpPkeyCtxPtr ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr));
  if (!ctx) {
    LogOpenSslError("RSA 密钥生成: 创建上下文失败");
    return false;
  }
  if (EVP_PKEY_keygen_init(ctx.get()) != 1) {
    LogOpenSslError("RSA 密钥生成: 初始化失败");
    return false;
  }
  if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), bits) != 1) {
    LogOpenSslError("RSA 密钥生成: 设置位数失败");
    return false;
  }
  EVP_PKEY* pkey = nullptr;
  if (EVP_PKEY_keygen(ctx.get(), &pkey) != 1) {
    LogOpenSslError("RSA 密钥生成: keygen 失败");
    return false;
  }
  out.pkey_.reset(pkey);
  return true;
}

bool RsaKey::SavePrivateKey(const std::string& path) const {
  if (!valid()) return false;
  FILE* fp = FileOpen(path, "wb");
  if (!fp) {
    LOG_ERROR("无法创建私钥文件: " << path);
    return false;
  }
  const int ok = PEM_write_PrivateKey(fp, pkey_.get(), nullptr, nullptr, 0,
                                      nullptr, nullptr);
  std::fclose(fp);
  if (ok != 1) {
    LogOpenSslError("保存私钥 PEM 失败");
    return false;
  }
  return true;
}

bool RsaKey::SavePublicKey(const std::string& path) const {
  if (!valid()) return false;
  FILE* fp = FileOpen(path, "wb");
  if (!fp) {
    LOG_ERROR("无法创建公钥文件: " << path);
    return false;
  }
  const int ok = PEM_write_PUBKEY(fp, pkey_.get());
  std::fclose(fp);
  if (ok != 1) {
    LogOpenSslError("保存公钥 PEM 失败");
    return false;
  }
  return true;
}

bool RsaKey::LoadPrivateKey(const std::string& path, RsaKey& out) {
  FILE* fp = FileOpen(path, "rb");
  if (!fp) {
    LOG_ERROR("无法打开私钥文件: " << path);
    return false;
  }
  EVP_PKEY* pkey = PEM_read_PrivateKey(fp, nullptr, nullptr, nullptr);
  std::fclose(fp);
  if (!pkey) {
    LogOpenSslError("加载私钥 PEM 失败");
    return false;
  }
  out.pkey_.reset(pkey);
  return true;
}

bool RsaKey::LoadPublicKey(const std::string& path, RsaKey& out) {
  FILE* fp = FileOpen(path, "rb");
  if (!fp) {
    LOG_ERROR("无法打开公钥文件: " << path);
    return false;
  }
  EVP_PKEY* pkey = PEM_read_PUBKEY(fp, nullptr, nullptr, nullptr);
  std::fclose(fp);
  if (!pkey) {
    LogOpenSslError("加载公钥 PEM 失败");
    return false;
  }
  out.pkey_.reset(pkey);
  return true;
}

// --------------------------- Rsa ---------------------------

bool Rsa::PublicEncrypt(EVP_PKEY* pubkey,
                        const std::vector<unsigned char>& plaintext,
                        std::vector<unsigned char>& ciphertext) {
  if (!pubkey) {
    LOG_ERROR("RSA 加密: 公钥为空");
    return false;
  }
  if (plaintext.size() > kMaxEncryptSize) {
    LOG_ERROR("RSA 加密: 明文过长 " << plaintext.size()
              << " 字节（上限 " << kMaxEncryptSize << "）");
    return false;
  }

  EvpPkeyCtxPtr ctx(EVP_PKEY_CTX_new(pubkey, nullptr));
  if (!ctx) {
    LogOpenSslError("RSA 加密: 创建上下文失败");
    return false;
  }
  if (EVP_PKEY_encrypt_init(ctx.get()) != 1) {
    LogOpenSslError("RSA 加密: 初始化失败");
    return false;
  }
  if (EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_OAEP_PADDING) != 1) {
    LogOpenSslError("RSA 加密: 设置 OAEP 填充失败");
    return false;
  }
  if (EVP_PKEY_CTX_set_rsa_oaep_md(ctx.get(), EVP_sha256()) != 1) {
    LogOpenSslError("RSA 加密: 设置 OAEP 摘要失败");
    return false;
  }

  std::size_t outlen = 0;
  if (EVP_PKEY_encrypt(ctx.get(), nullptr, &outlen,
                       plaintext.data(), plaintext.size()) != 1) {
    LogOpenSslError("RSA 加密: 查询输出长度失败");
    return false;
  }
  ciphertext.resize(outlen);
  if (EVP_PKEY_encrypt(ctx.get(), ciphertext.data(), &outlen,
                       plaintext.data(), plaintext.size()) != 1) {
    LogOpenSslError("RSA 加密: 加密失败");
    return false;
  }
  ciphertext.resize(outlen);
  return true;
}

bool Rsa::PrivateDecrypt(EVP_PKEY* privkey,
                         const std::vector<unsigned char>& ciphertext,
                         std::vector<unsigned char>& plaintext) {
  if (!privkey) {
    LOG_ERROR("RSA 解密: 私钥为空");
    return false;
  }
  if (ciphertext.empty()) {
    LOG_ERROR("RSA 解密: 密文为空");
    return false;
  }

  EvpPkeyCtxPtr ctx(EVP_PKEY_CTX_new(privkey, nullptr));
  if (!ctx) {
    LogOpenSslError("RSA 解密: 创建上下文失败");
    return false;
  }
  if (EVP_PKEY_decrypt_init(ctx.get()) != 1) {
    LogOpenSslError("RSA 解密: 初始化失败");
    return false;
  }
  if (EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_OAEP_PADDING) != 1) {
    LogOpenSslError("RSA 解密: 设置 OAEP 填充失败");
    return false;
  }
  if (EVP_PKEY_CTX_set_rsa_oaep_md(ctx.get(), EVP_sha256()) != 1) {
    LogOpenSslError("RSA 解密: 设置 OAEP 摘要失败");
    return false;
  }

  std::size_t outlen = 0;
  if (EVP_PKEY_decrypt(ctx.get(), nullptr, &outlen,
                       ciphertext.data(), ciphertext.size()) != 1) {
    // 常见原因：私钥与公钥不配对 / 密文被篡改（OAEP 解码失败）
    LogOpenSslError("RSA 解密: 解密失败（密钥不配对或密文损坏）");
    return false;
  }
  plaintext.resize(outlen);
  if (EVP_PKEY_decrypt(ctx.get(), plaintext.data(), &outlen,
                       ciphertext.data(), ciphertext.size()) != 1) {
    LogOpenSslError("RSA 解密: 解密失败");
    return false;
  }
  plaintext.resize(outlen);
  return true;
}

bool Rsa::Sign(EVP_PKEY* privkey,
               const std::vector<unsigned char>& data,
               std::vector<unsigned char>& signature) {
  if (!privkey) {
    LOG_ERROR("RSA 签名: 私钥为空");
    return false;
  }
  EvpMdCtxPtr ctx(EVP_MD_CTX_new());
  if (!ctx) {
    LogOpenSslError("RSA 签名: 创建上下文失败");
    return false;
  }
  if (EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr,
                         privkey) != 1) {
    LogOpenSslError("RSA 签名: 初始化失败");
    return false;
  }
  if (EVP_DigestSignUpdate(ctx.get(), data.data(), data.size()) != 1) {
    LogOpenSslError("RSA 签名: 摘要更新失败");
    return false;
  }
  std::size_t siglen = 0;
  if (EVP_DigestSignFinal(ctx.get(), nullptr, &siglen) != 1) {
    LogOpenSslError("RSA 签名: 查询长度失败");
    return false;
  }
  signature.resize(siglen);
  if (EVP_DigestSignFinal(ctx.get(), signature.data(), &siglen) != 1) {
    LogOpenSslError("RSA 签名: 签名失败");
    return false;
  }
  signature.resize(siglen);
  return true;
}

bool Rsa::Verify(EVP_PKEY* pubkey,
                 const std::vector<unsigned char>& data,
                 const std::vector<unsigned char>& signature) {
  if (!pubkey) {
    LOG_ERROR("RSA 验签: 公钥为空");
    return false;
  }
  if (signature.empty()) {
    LOG_ERROR("RSA 验签: 签名为空");
    return false;
  }
  EvpMdCtxPtr ctx(EVP_MD_CTX_new());
  if (!ctx) {
    LogOpenSslError("RSA 验签: 创建上下文失败");
    return false;
  }
  if (EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr,
                           pubkey) != 1) {
    LogOpenSslError("RSA 验签: 初始化失败");
    return false;
  }
  if (EVP_DigestVerifyUpdate(ctx.get(), data.data(), data.size()) != 1) {
    LogOpenSslError("RSA 验签: 摘要更新失败");
    return false;
  }
  // 1=验签成功 0=验签失败(篡改/密钥不匹配) -1=参数错误
  // 验签失败是业务层正常结果（用于篡改检测），不视为系统错误
  return EVP_DigestVerifyFinal(ctx.get(), signature.data(),
                               signature.size()) == 1;
}

}  // namespace mail
