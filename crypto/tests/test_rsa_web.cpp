// test_rsa_web.cpp —— Web↔服务器 RSA 信封单元测试（第一层加密，OpenSSL 库）
//
// 覆盖：
//   1) 服务器 RSA-2048 密钥自动生成 / 公钥 PEM 下发
//   2) 浏览器→服务器：信封（AES-256-GCM + RSA-OAEP-SHA256）Seal→Open 往返一致
//   3) 服务器→浏览器：用"收信方(浏览器)公钥"封的信封，可被浏览器侧私钥解开
//      （此处用 OpenSSL 私钥 + EVP AES-GCM 模拟浏览器 WebCrypto 的解封路径）
//   4) 篡改密文 / 用错误私钥拆封 → 必须失败
//
// 编译：make test-rsa     运行：./crypto/tests/test_rsa_web
#include "crypto/rsa.hpp"
#include "MailCrypto.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <iostream>
#include <string>
#include <vector>

static int g_fail = 0;
static void Check(bool ok, const char* name) {
  std::cout << "  [" << (ok ? "OK" : "FAIL") << "] " << name << std::endl;
  if (!ok) ++g_fail;
}

static std::string B64E(const std::string& d) {
  static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  for (size_t i = 0; i < d.size(); i += 3) {
    unsigned a = (unsigned char)d[i];
    unsigned b = (i+1<d.size()) ? (unsigned char)d[i+1] : 0;
    unsigned c = (i+2<d.size()) ? (unsigned char)d[i+2] : 0;
    unsigned n = (a<<16)|(b<<8)|c;
    o += t[(n>>18)&63]; o += t[(n>>12)&63];
    o += (i+1<d.size()) ? t[(n>>6)&63] : '=';
    o += (i+2<d.size()) ? t[n&63] : '=';
  }
  return o;
}
static std::string B64D(const std::string& s) {
  auto v = [](char c) -> int {
    if (c>='A'&&c<='Z') return c-'A'; if (c>='a'&&c<='z') return c-'a'+26;
    if (c>='0'&&c<='9') return c-'0'+52; if (c=='+') return 62;
    if (c=='/') return 63; return -1; };
  std::string o; int buf=0, bits=0;
  for (char c : s) {
    if (c=='\r'||c=='\n'||c==' ') continue;
    if (c=='=') break;
    int x=v(c); if (x<0) continue;
    buf=(buf<<6)|x; bits+=6;
    if (bits>=8){ bits-=8; o+=(char)((buf>>bits)&0xFF); }
  }
  return o;
}

// 模拟浏览器侧：RSA-OAEP 私钥解会话密钥 + AES-256-GCM 解密（与 WebCrypto 等价）
static bool BrowserUnseal(mail::RsaKey& browserKey, const std::string& packed,
                          std::string& plain) {
  auto field = [&](const std::string& n) {
    std::string p = n + "=";
    size_t pos = packed.find(p);
    if (pos == std::string::npos) return std::string();
    pos += p.size();
    size_t end = packed.find('|', pos);
    return (end == std::string::npos) ? packed.substr(pos) : packed.substr(pos, end-pos);
  };
  std::string k = B64D(field("k")), iv = B64D(field("iv")), ct = B64D(field("ct"));
  if (k.empty() || iv.size()<8 || ct.size()<16) return false;

  std::vector<unsigned char> raw;
  std::vector<unsigned char> kv(k.begin(), k.end());
  if (!mail::Rsa::PrivateDecrypt(browserKey.get(), kv, raw) || raw.size() != 32)
    return false;

  const int tagLen = 16;
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;
  int ok = 1, len = 0, total = 0;
  ok = ok && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
  ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)iv.size(), nullptr);
  ok = ok && EVP_DecryptInit_ex(ctx, nullptr, nullptr, raw.data(),
                                reinterpret_cast<const unsigned char*>(iv.data()));
  std::string out(ct.size()+16, '\0');
  ok = ok && EVP_DecryptUpdate(ctx, (unsigned char*)&out[0], &len,
      reinterpret_cast<const unsigned char*>(ct.data()), (int)(ct.size()-tagLen));
  total = len;
  ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tagLen,
      (void*)(ct.data()+ct.size()-tagLen));
  int flen = 0;
  ok = ok && EVP_DecryptFinal_ex(ctx, (unsigned char*)&out[total], &flen);
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) return false;
  out.resize((size_t)(total+flen));
  plain.swap(out);
  return true;
}

int main() {
  const std::string keyDir = "./keys";
  std::cout << "== Web↔服务器 RSA 信封单元测试 ==" << std::endl;

  Check(MailCrypto::ensureWebRsaKeys(keyDir), "服务器 RSA-2048 密钥自动生成");
  std::string srvPub;
  Check(MailCrypto::webServerPublicKeyPem(srvPub, keyDir)
        && srvPub.find("BEGIN PUBLIC KEY") != std::string::npos,
        "服务器公钥 PEM 可读（BEGIN PUBLIC KEY）");

  // 模拟浏览器生成自己的 RSA-OAEP-2048 密钥对
  mail::RsaKey browserKey;
  Check(mail::RsaKey::Generate(browserKey, 2048), "模拟浏览器 RSA 密钥生成");
  BIO* bio = BIO_new(BIO_s_mem());
  PEM_write_bio_PUBKEY(bio, browserKey.get());
  char* p = nullptr; long n = BIO_get_mem_data(bio, &p);
  std::string browserPubPem(p, (size_t)n); BIO_free(bio);

  const std::string plain =
      "to=bob@example.com&subject=%E6%9C%BA%E5%AF%86&body=hello&algo=aes&encrypt=1";

  // 2) 浏览器→服务器
  std::string reqEnv;
  Check(MailCrypto::webEnvelopeSeal(plain, srvPub, reqEnv), "浏览器→服务器 信封 Seal");
  std::string out;
  Check(MailCrypto::webEnvelopeOpen(reqEnv, out) && out == plain,
        "服务器私钥 Open 一致");

  // 3) 服务器→浏览器
  std::string respEnv;
  Check(MailCrypto::webEnvelopeSeal(plain, browserPubPem, respEnv),
        "服务器→浏览器 信封 Seal（浏览器公钥）");
  std::string plain2;
  Check(BrowserUnseal(browserKey, respEnv, plain2) && plain2 == plain,
        "浏览器侧私钥+WebCrypto路径 解封一致");

  // 4) 防篡改
  std::string bad = reqEnv;
  size_t pos = bad.find("|ct=");
  bad[pos+4] = (bad[pos+4]=='A') ? 'B' : 'A';
  std::string junk;
  Check(!MailCrypto::webEnvelopeOpen(bad, junk), "篡改密文 → 解密失败(GCM/篡改检测)");
  mail::RsaKey wrongKey;
  mail::RsaKey::Generate(wrongKey, 2048);
  BIO* b2 = BIO_new(BIO_s_mem());
  PEM_write_bio_PUBKEY(b2, wrongKey.get());
  char* p2 = nullptr; long n2 = BIO_get_mem_data(b2, &p2);
  std::string wrongPub(p2, (size_t)n2); BIO_free(b2);
  std::string junk2;
  std::string envWrong;
  MailCrypto::webEnvelopeSeal(plain, wrongPub, envWrong);
  Check(!MailCrypto::webEnvelopeOpen(envWrong, junk2), "用错误服务器公钥 → 拆封失败");

  std::cout << (g_fail ? "===== 有失败项 =====" : "===== 全部通过 =====") << std::endl;
  return g_fail;
}
