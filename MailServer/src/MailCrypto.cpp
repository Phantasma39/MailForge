// ============================================================================
//  MailCrypto.cpp —— 邮件加密模块实现
//
//  结构：
//    1) 基础工具：Base64（RFC 4648，76 字符换行）
//    2) ★ 自研对称加密通道：AES-256-CBC 与 ChaCha20（RFC 8439），原语位于
//       crypto/aes.* 与 crypto/chacha20.*（纯自研，无 OpenSSL）
//    3) 按账号对称密钥管理：keys/<用户名>.key（32 字节，不存在则自动生成）
// ============================================================================

#include "MailCrypto.h"

#include <sys/stat.h>
#include <cctype>
#include <mutex>
#include <fstream>
#include <iostream>
#include <vector>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>

#include "crypto/aes.hpp"
#include "crypto/chacha20.hpp"
#include "crypto/random.hpp"
#include "crypto/rsa.hpp"

namespace {

std::mutex gKeyMutex;   // 密钥“先查文件再写”，用锁避免多线程并发写坏

// 邮箱地址 → 本地用户名：bob@example.com / Bob / bob → bob
std::string normUser(const std::string& email) {
    std::string u = email;
    std::size_t at = u.find('@');
    if (at != std::string::npos) u = u.substr(0, at);
    std::string out;
    for (char c : u) {
        if (isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_') {
            out += (char)tolower((unsigned char)c);
        }
    }
    return out;
}

bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

std::string userKeyPath(const std::string& user, const std::string& dir) {
    return dir + "/" + user + ".key";
}

bool readRawFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign((std::istreambuf_iterator<char>(in)),
               std::istreambuf_iterator<char>());
    return true;
}

bool writeRawFile(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), (std::streamsize)data.size());
    out.close();
    return true;
}

// 幂等：确保 user 的对称密钥存在并读出来（32 字节）
bool loadOrCreateUserKey(const std::string& user, const std::string& keyDir,
                         std::string& keyOut) {
    std::lock_guard<std::mutex> lock(gKeyMutex);

    if (!keyDir.empty()) {
        struct stat st;
        if (stat(keyDir.c_str(), &st) != 0 && mkdir(keyDir.c_str(), 0755) != 0) {
            return false;
        }
    }

    const std::string path = userKeyPath(user, keyDir);
    if (fileExists(path)) {
        if (!readRawFile(path, keyOut)) return false;
        if (keyOut.size() != 32) {
            std::cerr << "[MailCrypto] 密钥文件长度非法: " << path
                      << "（期望 32 字节，实际 " << keyOut.size() << "）"
                      << std::endl;
            return false;
        }
        return true;
    }

    // 不存在：生成 32 字节随机密钥并落盘
    std::string key(32, '\0');
    if (!mail::RandomBytes(reinterpret_cast<unsigned char*>(&key[0]),
                           key.size())) {
        std::cerr << "[MailCrypto] 随机源不可用，无法生成密钥" << std::endl;
        return false;
    }
    if (!writeRawFile(path, key)) {
        std::cerr << "[MailCrypto] 无法写入密钥文件: " << path << std::endl;
        return false;
    }
    keyOut.swap(key);
    return true;
}

// ============================================================================
//  Web↔服务器 RSA 信封的底层工具（OpenSSL 库：AES-256-GCM + RSA-OAEP）
// ============================================================================

// AES-256-GCM 加密：输出 = 密文 ‖ 16B tag（GCM 是认证加密，tag 校验即防篡改）
bool AesGcmEncrypt(const std::string& key, const std::string& iv,
                   const std::string& plain, std::string& cipherTag) {
    if (key.size() != 32 || iv.size() < 8) return false;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    int ok = 1, len = 0, total = 0;
    ok = ok && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                                   (int)iv.size(), nullptr);
    ok = ok && EVP_EncryptInit_ex(ctx, nullptr, nullptr,
        (const unsigned char*)key.data(), (const unsigned char*)iv.data());
    std::string ct(plain.size() + 16, '\0');
    ok = ok && EVP_EncryptUpdate(ctx, (unsigned char*)&ct[0], &len,
        (const unsigned char*)plain.data(), (int)plain.size());
    total = len;
    ok = ok && EVP_EncryptFinal_ex(ctx, (unsigned char*)&ct[total], &len);
    total += len;
    char tag[16];
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return false;
    cipherTag.assign(ct.data(), (size_t)total);
    cipherTag.append(tag, 16);
    return true;
}

// AES-256-GCM 解密：输入 = 密文 ‖ 16B tag；tag 校验失败（篡改/密钥错）返回 false
bool AesGcmDecrypt(const std::string& key, const std::string& iv,
                   const std::string& cipherTag, std::string& plain) {
    if (key.size() != 32 || cipherTag.size() < 16 + 16) return false;
    const int tagLen = 16;
    const size_t ctLen = cipherTag.size() - (size_t)tagLen;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    int ok = 1, len = 0, total = 0;
    ok = ok && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                                   (int)iv.size(), nullptr);
    ok = ok && EVP_DecryptInit_ex(ctx, nullptr, nullptr,
        (const unsigned char*)key.data(), (const unsigned char*)iv.data());
    std::string out(cipherTag.size() + 16, '\0');
    ok = ok && EVP_DecryptUpdate(ctx, (unsigned char*)&out[0], &len,
        (const unsigned char*)cipherTag.data(), (int)ctLen);
    total = len;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tagLen,
                                   (void*)(cipherTag.data() + ctLen));
    int flen = 0;
    ok = ok && EVP_DecryptFinal_ex(ctx, (unsigned char*)&out[total], &flen);
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return false;
    out.resize((size_t)(total + flen));
    plain.swap(out);
    return true;
}

// PEM 公钥字符串 → EVP_PKEY*（PKCS#8 SubjectPublicKeyInfo，与 WebCrypto spki 兼容）
EVP_PKEY* PubFromPem(const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), (int)pem.size());
    if (!bio) return nullptr;
    EVP_PKEY* key = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return key;
}

// 从 "k=xx|iv=yy|ct=zz" 信封文本中取某个字段值
bool EnvField(const std::string& packed, const std::string& name,
              std::string& val) {
    const std::string p = name + "=";
    size_t pos = packed.find(p);
    if (pos == std::string::npos) return false;
    pos += p.size();
    size_t end = packed.find('|', pos);
    val = (end == std::string::npos) ? packed.substr(pos)
                                     : packed.substr(pos, end - pos);
    return !val.empty();
}

} // namespace

namespace MailCrypto {

const char* kEncMagicAes = "MailForge::ENC::AES::";
const char* kEncMagicCha = "MailForge::ENC::CHA::";

static bool startsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() &&
           text.compare(0, prefix.size(), prefix) == 0;
}

CryptoAlgo detectAlgo(const std::string& text) {
    if (startsWith(text, kEncMagicAes)) return ALGO_AES_CBC;
    if (startsWith(text, kEncMagicCha)) return ALGO_CHACHA20;
    return ALGO_NONE;
}

bool isEncryptedText(const std::string& text) {
    return detectAlgo(text) != ALGO_NONE;
}

// ==================== Base64（RFC 4648，76 字符换行） ====================

std::string base64Encode(const std::string& data) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4 + 16);
    size_t col = 0;
    for (size_t i = 0; i < data.size(); i += 3) {
        unsigned a = (unsigned char)data[i];
        unsigned b = (i + 1 < data.size()) ? (unsigned char)data[i + 1] : 0;
        unsigned c = (i + 2 < data.size()) ? (unsigned char)data[i + 2] : 0;
        unsigned n = (a << 16) | (b << 8) | c;
        out += tbl[(n >> 18) & 63];
        out += tbl[(n >> 12) & 63];
        out += (i + 1 < data.size()) ? tbl[(n >> 6) & 63] : '=';
        out += (i + 2 < data.size()) ? tbl[n & 63] : '=';
        col += 4;
        // SMTP 单行 ≤1000 字节：Base64 每 76 字符换行（解码忽略换行）
        if (col >= 76) { out += "\r\n"; col = 0; }
    }
    return out;
}

std::string base64Decode(const std::string& text) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;   // '=' 或非法字符
    };
    std::string out;
    int buf = 0, bits = 0;
    for (char c : text) {
        if (c == '\r' || c == '\n' || c == ' ') continue;   // 忽略换行
        if (c == '=') break;                                // 填充结束
        int v = val(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)((buf >> bits) & 0xFF); }
    }
    return out;
}

// ==================== 加解密主入口 ====================

std::string encryptPayload(const std::string& plainText,
                           const std::string& key,
                           CryptoAlgo algo) {
    switch (algo) {
        case ALGO_NONE:
            return plainText;   // 明文直传

        case ALGO_AES_CBC: {
            if (key.size() != 32) return "";   // 密钥长度不对 = 加密失败信号
            std::vector<unsigned char> kvec(key.begin(), key.end());
            std::vector<unsigned char> pt(plainText.begin(), plainText.end());
            std::vector<unsigned char> iv = mail::Aes::GenerateIv();
            std::vector<unsigned char> ct;
            if (!mail::Aes::Encrypt(kvec, iv, pt, ct)) return "";
            // 线格式：IV(16字节) ‖ 密文 → Base64
            std::string raw;
            raw.append((const char*)iv.data(), iv.size());
            raw.append((const char*)ct.data(), ct.size());
            return std::string(kEncMagicAes) + base64Encode(raw);
        }

        case ALGO_CHACHA20: {
            if (key.size() != 32) return "";
            std::vector<unsigned char> kvec(key.begin(), key.end());
            std::vector<unsigned char> pt(plainText.begin(), plainText.end());
            std::vector<unsigned char> nonce(12, 0);
            if (!mail::RandomBytes(nonce.data(), nonce.size())) return "";
            std::vector<unsigned char> ct;
            if (!mail::ChaCha20::Crypt(kvec, nonce, pt, ct)) return "";
            // 线格式：nonce(12字节) ‖ 密文 → Base64
            std::string raw;
            raw.append((const char*)nonce.data(), nonce.size());
            raw.append((const char*)ct.data(), ct.size());
            return std::string(kEncMagicCha) + base64Encode(raw);
        }

        default:
            return plainText;   // 未知算法兜底：当明文处理
    }
}

// 解密结果应是一封可展示的邮件载荷（以 "Subject:" 开头）。密钥错误时解出
// 乱码会在这里被拦下——尤其 ChaCha20 这类无填充校验的流密码需要此兜底。
static bool looksLikeMailPayload(const std::string& s) {
    if (s.size() < 10) return false;
    for (int i = 0; i < 8; ++i) {
        if (tolower((unsigned char)s[i]) != "subject:"[i]) return false;
    }
    return true;
}

bool decryptPayload(const std::string& cipherText,
                    const std::string& key,
                    std::string& plainOut) {
    const CryptoAlgo algo = detectAlgo(cipherText);
    if (algo == ALGO_NONE) {
        plainOut = cipherText;   // 本来就没加密
        return true;
    }

    if (key.size() != 32) return false;
    const std::vector<unsigned char> kvec(key.begin(), key.end());

    if (algo == ALGO_AES_CBC) {
        const std::string b64 =
            cipherText.substr(std::string(kEncMagicAes).size());
        const std::string raw = base64Decode(b64);
        if (raw.size() < 16 + 16) return false;   // IV + 至少一个密文块
        std::vector<unsigned char> iv(raw.begin(), raw.begin() + 16);
        std::vector<unsigned char> ct(raw.begin() + 16, raw.end());
        std::vector<unsigned char> pt;
        if (!mail::Aes::Decrypt(kvec, iv, ct, pt)) return false;
        std::string out(pt.begin(), pt.end());
        if (!looksLikeMailPayload(out)) return false;
        plainOut.swap(out);
        return true;
    }

    if (algo == ALGO_CHACHA20) {
        const std::string b64 =
            cipherText.substr(std::string(kEncMagicCha).size());
        const std::string raw = base64Decode(b64);
        if (raw.size() < 12) return false;   // nonce
        std::vector<unsigned char> nonce(raw.begin(), raw.begin() + 12);
        std::vector<unsigned char> ct(raw.begin() + 12, raw.end());
        std::vector<unsigned char> pt;
        if (!mail::ChaCha20::Crypt(kvec, nonce, ct, pt)) return false;
        std::string out(pt.begin(), pt.end());
        if (!looksLikeMailPayload(out)) return false;   // 流密码靠载荷头兜底
        plainOut.swap(out);
        return true;
    }

    return false;
}

// ==================== 按账号对称密钥管理 ====================

bool getUserKey(const std::string& userKey, std::string& keyOut,
                const std::string& keyDir) {
    const std::string u = normUser(userKey);
    if (u.empty()) return false;
    return loadOrCreateUserKey(u, keyDir, keyOut);
}

bool ensureUserKey(const std::string& userKey, const std::string& keyDir) {
    std::string dummy;
    return getUserKey(userKey, dummy, keyDir);
}

// ============================================================================
//  Web↔服务器 RSA 信封实现（第一层：浏览器 ↔ HTTP 服务器，OpenSSL 库）
// ============================================================================

namespace {
// 确保 keyDir 存在
bool EnsureDir(const std::string& dir) {
    if (dir.empty()) return true;
    struct stat st;
    if (stat(dir.c_str(), &st) != 0 && mkdir(dir.c_str(), 0755) != 0) {
        std::cerr << "[MailCrypto] 无法创建目录: " << dir << std::endl;
        return false;
    }
    return true;
}
} // namespace

bool ensureWebRsaKeys(const std::string& keyDir) {
    if (!EnsureDir(keyDir)) return false;
    const std::string privPath = keyDir + "/server_private.pem";
    const std::string pubPath  = keyDir + "/server_public.pem";

    if (fileExists(privPath) && fileExists(pubPath)) return true;
    if (fileExists(privPath) && !fileExists(pubPath)) {
        // 私钥在、公钥丢：从私钥补导出（PEM_write_PUBKEY 写的就是 SubjectPublicKeyInfo）
        mail::RsaKey kp;
        if (!mail::RsaKey::LoadPrivateKey(privPath, kp)) return false;
        FILE* f = fopen(pubPath.c_str(), "wb");
        if (!f) return false;
        const bool ok = (PEM_write_PUBKEY(f, kp.get()) == 1);
        fclose(f);
        return ok;
    }
    mail::RsaKey kp;
    if (!mail::RsaKey::Generate(kp, 2048)) return false;
    if (!kp.SavePrivateKey(privPath)) return false;
    if (!kp.SavePublicKey(pubPath)) return false;
    std::cout << "[MailCrypto] 已生成服务器 Web RSA-2048 密钥对: "
              << keyDir << "/server_private.pem / server_public.pem" << std::endl;
    return true;
}

bool webServerPublicKeyPem(std::string& pemOut, const std::string& keyDir) {
    if (!ensureWebRsaKeys(keyDir)) return false;
    return readRawFile(keyDir + "/server_public.pem", pemOut);
}

bool webEnvelopeOpen(const std::string& packed, std::string& plainOut,
                     const std::string& keyDir) {
    plainOut.clear();
    if (!ensureWebRsaKeys(keyDir)) return false;

    std::string kb64, ivb64, ctb64;
    if (!EnvField(packed, "k", kb64) || !EnvField(packed, "iv", ivb64) ||
        !EnvField(packed, "ct", ctb64)) {
        std::cerr << "[MailCrypto] Web 信封格式非法" << std::endl;
        return false;
    }

    const std::string sessionKeyB64 = kb64;   // RSA-OAEP 包装后的会话密钥
    const std::string encKey = base64Decode(sessionKeyB64);
    const std::string iv     = base64Decode(ivb64);
    const std::string ctTag  = base64Decode(ctb64);
    if (encKey.empty() || iv.size() < 8 || ctTag.size() < 16) {
        std::cerr << "[MailCrypto] Web 信封字段解码失败" << std::endl;
        return false;
    }

    // 1) 服务器私钥解开会话密钥
    mail::RsaKey priv;
    if (!mail::RsaKey::LoadPrivateKey(keyDir + "/server_private.pem", priv)) {
        std::cerr << "[MailCrypto] 加载服务器私钥失败" << std::endl;
        return false;
    }
    std::vector<unsigned char> sessionKey;
    std::vector<unsigned char> encVec(encKey.begin(), encKey.end());
    if (!mail::Rsa::PrivateDecrypt(priv.get(), encVec, sessionKey)) {
        std::cerr << "[MailCrypto] RSA-OAEP 解会话密钥失败" << std::endl;
        return false;
    }
    if (sessionKey.size() != 32) {
        std::cerr << "[MailCrypto] 会话密钥长度异常" << std::endl;
        return false;
    }
    const std::string key(sessionKey.begin(), sessionKey.end());

    // 2) AES-256-GCM 解正文（tag 校验失败即视为解密失败）
    return AesGcmDecrypt(key, iv, ctTag, plainOut);
}

bool webEnvelopeSeal(const std::string& plain,
                     const std::string& recipientPublicPem,
                     std::string& packedOut) {
    packedOut.clear();

    EVP_PKEY* pub = PubFromPem(recipientPublicPem);
    if (!pub) {
        std::cerr << "[MailCrypto] Web 信封封装：无法解析收信方 RSA 公钥 PEM" << std::endl;
        return false;
    }

    // 1) 随机 AES-256-GCM 会话密钥 + 12B IV
    std::string key(32, '\0'), iv(12, '\0');
    if (!mail::RandomBytes(reinterpret_cast<unsigned char*>(&key[0]), key.size()) ||
        !mail::RandomBytes(reinterpret_cast<unsigned char*>(&iv[0]), iv.size())) {
        EVP_PKEY_free(pub);
        return false;
    }

    // 2) GCM 加密正文
    std::string ctTag;
    if (!AesGcmEncrypt(key, iv, plain, ctTag)) {
        EVP_PKEY_free(pub);
        return false;
    }

    // 3) RSA-OAEP 封装会话密钥（用收信方公钥）
    std::vector<unsigned char> keyVec(key.begin(), key.end());
    std::vector<unsigned char> wrapped;
    if (!mail::Rsa::PublicEncrypt(pub, keyVec, wrapped)) {
        std::cerr << "[MailCrypto] Web 信封封装：RSA-OAEP 封装会话密钥失败" << std::endl;
        EVP_PKEY_free(pub);
        return false;
    }
    EVP_PKEY_free(pub);

    packedOut = "k=" + base64Encode(std::string(wrapped.begin(), wrapped.end()))
              + "|iv=" + base64Encode(iv)
              + "|ct=" + base64Encode(ctTag);
    return true;
}

} // namespace MailCrypto
