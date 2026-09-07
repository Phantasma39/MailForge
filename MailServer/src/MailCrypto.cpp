// ============================================================================
//  MailCrypto.cpp —— 邮件加密模块实现
//
//  内容分两大部分：
//    1) 自带的轻量算法：Base64 + XOR（可插拔入口 encryptPayload / decryptPayload，
//       保留用于兼容历史上用 XOR 通道发的旧邮件与附件编解码）；
//    2) ★ 数字信封（AES-256-CBC + RSA-2048）：对接合并进来的 crypto-project 子系统
//       （mail::DigitalEnvelope / mail::RsaKey，见仓库 crypto/、common/ 目录），
//       为 MailForge 的 SMTP / POP3 / HTTP 收发链路提供非对称混合加密通道。
// ============================================================================

#include "MailCrypto.h"

#include <sys/stat.h>
#include <cctype>
#include <mutex>
#include <iostream>
#include <openssl/pem.h>

#include "crypto/envelope.hpp"
#include "crypto/rsa.hpp"

namespace {

// ---- 数字信封用到的 RSA 密钥管理工具 ----
// 密钥文件放 keyDir（默认 ./keys；已在 .gitignore 中忽略 keys/ 与 *.pem）

std::mutex gKeyMutex;   // 密钥生成是"先查文件再写"，用锁避免多线程并发写坏

// 邮箱地址 → 本地用户名：bob@example.com / Bob / bob → bob
// （只保留字母数字与 _-.，防止路径字符把密钥写到别处）
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

std::string privKeyPath(const std::string& userKey, const std::string& dir) {
    return dir + "/" + userKey + ".key.pem";
}
std::string pubKeyPath(const std::string& userKey, const std::string& dir) {
    return dir + "/" + userKey + ".pub.pem";
}

// PEM 私钥文件通常同时含公钥，这里把它导出成独立的公钥文件
bool exportPublicFromPrivate(EVP_PKEY* priv, const std::string& pubPath) {
    FILE* f = fopen(pubPath.c_str(), "wb");
    if (!f) return false;
    const bool ok = (PEM_write_PUBKEY(f, priv) == 1);
    fclose(f);
    return ok;
}

// 幂等：确保 userKey 的 RSA-2048 密钥对存在，并加载出来
bool loadOrCreateKeyPair(const std::string& userKey,
                         const std::string& keyDir,
                         mail::RsaKey& privOut,
                         mail::RsaKey& pubOut) {
    std::lock_guard<std::mutex> lock(gKeyMutex);

    if (!keyDir.empty()) {
        struct stat st;
        if (stat(keyDir.c_str(), &st) != 0 && mkdir(keyDir.c_str(), 0755) != 0) {
            return false;
        }
    }

    const std::string privP = privKeyPath(userKey, keyDir);
    const std::string pubP  = pubKeyPath(userKey, keyDir);
    const bool hasPriv = fileExists(privP);
    const bool hasPub  = fileExists(pubP);

    if (!hasPriv && !hasPub) {
        // 都没有：全新生成一对
        mail::RsaKey kp;
        if (!mail::RsaKey::Generate(kp)) return false;
        if (!kp.SavePrivateKey(privP)) return false;
        if (!kp.SavePublicKey(pubP)) return false;
    } else if (hasPriv && !hasPub) {
        // 私钥在、公钥丢了：从私钥补导出公钥，避免密钥对变化导致旧邮件拆不开
        mail::RsaKey kp;
        if (!mail::RsaKey::LoadPrivateKey(privP, kp)) return false;
        if (!exportPublicFromPrivate(kp.get(), pubP)) return false;
    } else if (!hasPriv && hasPub) {
        // 只有公钥：上次生成没写完，重新生成整对
        mail::RsaKey kp;
        if (!mail::RsaKey::Generate(kp)) return false;
        if (!kp.SavePrivateKey(privP)) return false;
        if (!kp.SavePublicKey(pubP)) return false;
    }

    if (!mail::RsaKey::LoadPrivateKey(privP, privOut)) return false;
    if (!mail::RsaKey::LoadPublicKey(pubP, pubOut)) return false;
    return true;
}

} // namespace

namespace MailCrypto {

const char* kEncMagicXor = "MailForge::ENC::XOR::";

// ==================== Base64（RFC 4648） ====================

std::string base64Encode(const std::string& data) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4 + 16);

    size_t col = 0;   // 当前行字符计数：每 76 个字符插一个 \r\n
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
        // SMTP 规定单行不能超过 1000 字节，Base64 每 76 字符换行（解码时会忽略换行）
        if (col >= 76) {
            out += "\r\n";
            col = 0;
        }
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
        // 编码时为了不超 SMTP 单行长度，每 76 字符插了一个换行；解码时直接跳过
        if (c == '\r' || c == '\n' || c == ' ') continue;
        if (c == '=') break;          // 填充符：Base64 数据到此结束
        int v = val(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((buf >> bits) & 0xFF);
        }
    }
    return out;
}

// ==================== XOR 异或 ====================

std::string xorCipher(const std::string& data, const std::string& key) {
    std::string out = data;
    if (key.empty()) return out;   // 没有密钥 = 不处理（安全起见）
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = (char)((unsigned char)out[i] ^ (unsigned char)key[i % key.size()]);
    }
    return out;
}

// ==================== 主入口 ====================

std::string encryptPayload(const std::string& plainText,
                           const std::string& key,
                           CryptoAlgo algo) {
    switch (algo) {
        case ALGO_NONE:
            return plainText;                       // 明文直传

        case ALGO_XOR:
            // 格式：签名头 + Base64(XOR(明文, key))
            return std::string(kEncMagicXor) + base64Encode(xorCipher(plainText, key));

        // 说明：ALGO_RC4 / ALGO_AES_CBC 为预留枚举位；当前主加密通道是
        // ALGO_ENVELOPE（数字信封 AES-256-CBC + RSA-2048），见本文件下方
        // encryptEnvelope / decryptEnvelope（已接入 HTTP 收发链路）。

        default:
            return plainText;   // 未知算法兜底：当明文处理
    }
}

std::string decryptPayload(const std::string& cipherText,
                           const std::string& key) {
    const std::string magic(kEncMagicXor);

    // 没有签名头 = 本来就没加密，原样返回
    if (cipherText.compare(0, magic.size(), magic) != 0) {
        return cipherText;
    }

    // 有 XOR 签名头：去掉头 → Base64 解码 → XOR 还原
    std::string b64 = cipherText.substr(magic.size());
    return xorCipher(base64Decode(b64), key);

    // 说明：当前新的加密邮件走下方 decryptEnvelope（数字信封），不会出现在此分支。
}

// ============================================================================
//  数字信封（AES-256-CBC + RSA-2048）—— 与 crypto-project 子系统对接的主通道
// ============================================================================

bool ensureUserKeyPair(const std::string& userKey, const std::string& keyDir) {
    const std::string u = normUser(userKey);
    if (u.empty()) {
        std::cerr << "[MailCrypto] 用户名为空，无法初始化密钥: \""
                  << userKey << "\"" << std::endl;
        return false;
    }
    mail::RsaKey priv, pub;
    return loadOrCreateKeyPair(u, keyDir, priv, pub);
}

bool encryptEnvelope(const std::string& plainText,
                     const std::string& from,
                     const std::string& to,
                     std::string& envelopeText,
                     const std::string& keyDir) {
    envelopeText.clear();

    const std::string fromUser = normUser(from);
    const std::string toUser   = normUser(to);
    if (fromUser.empty() || toUser.empty()) {
        std::cerr << "[MailCrypto] 信封加密失败：发件人/收件人地址不合法 (from="
                  << from << ", to=" << to << ")" << std::endl;
        return false;
    }

    // 发件人密钥对（签名）与收件人密钥对（加密会话密钥），缺则自动生成
    mail::RsaKey senderPriv, senderPub, recipPriv, recipPub;
    if (!loadOrCreateKeyPair(fromUser, keyDir, senderPriv, senderPub)) {
        std::cerr << "[MailCrypto] 信封加密失败：无法获取发件人 "
                  << fromUser << " 的密钥" << std::endl;
        return false;
    }
    if (!loadOrCreateKeyPair(toUser, keyDir, recipPriv, recipPub)) {
        std::cerr << "[MailCrypto] 信封加密失败：无法获取收件人 "
                  << toUser << " 的密钥" << std::endl;
        return false;
    }

    // 用收件人公钥 Seal：AES 加密正文 + RSA 加密会话密钥 + 发件人私钥签名
    const std::vector<unsigned char> pt(plainText.begin(), plainText.end());
    mail::Envelope env;
    if (!mail::DigitalEnvelope::Seal(pt, from, to,
                                     senderPriv.get(), recipPub.get(), env)) {
        std::cerr << "[MailCrypto] 信封加密失败：数字信封 Seal 出错" << std::endl;
        return false;
    }

    // 序列化成 ASCII 信封文本（可原样放进 SMTP DATA 传输）
    if (!mail::DigitalEnvelope::Serialize(env, envelopeText)) {
        std::cerr << "[MailCrypto] 信封加密失败：信封序列化出错" << std::endl;
        envelopeText.clear();
        return false;
    }
    return true;
}

bool decryptEnvelope(const std::string& envelopeText,
                     const std::string& viewerKey,
                     std::string& plainText,
                     const std::string& keyDir) {
    plainText.clear();
    if (!isEnvelopeText(envelopeText)) return false;

    const std::string viewer = normUser(viewerKey);
    if (viewer.empty()) return false;

    // 1) 解析 ASCII 信封
    mail::Envelope env;
    if (!mail::DigitalEnvelope::Parse(envelopeText, env)) {
        std::cerr << "[MailCrypto] 信封解密失败：信封解析出错" << std::endl;
        return false;
    }

    // 2) 收件人必须有自己的私钥，否则拆不开信封
    const std::string privP = privKeyPath(viewer, keyDir);
    if (!fileExists(privP)) {
        std::cerr << "[MailCrypto] 信封解密失败：用户 " << viewer
                  << " 没有私钥（请先让该账号登录一次以自动生成密钥）" << std::endl;
        return false;
    }
    mail::RsaKey viewerPriv;
    if (!mail::RsaKey::LoadPrivateKey(privP, viewerPriv)) return false;

    // 3) 发送方公钥在本地存在则验签；信封本身未签名则只解密不验签
    EVP_PKEY* verifyKey = nullptr;
    mail::RsaKey senderPub;
    const std::string senderUser = normUser(env.from);
    if (!senderUser.empty() && !env.signature.empty()) {
        mail::RsaKey tmp;
        if (mail::RsaKey::LoadPublicKey(pubKeyPath(senderUser, keyDir), tmp)) {
            senderPub = std::move(tmp);
            verifyKey = senderPub.get();
        }
    }

    std::vector<unsigned char> out;
    if (!mail::DigitalEnvelope::Open(env, viewerPriv.get(), verifyKey, out)) {
        std::cerr << "[MailCrypto] 信封解密失败：Open 出错"
                     "（收件人密钥不匹配 / 内容被篡改 / 签名不符？）" << std::endl;
        return false;
    }
    plainText.assign(out.begin(), out.end());
    return true;
}

bool isEnvelopeText(const std::string& text) {
    return text.find("-----BEGIN MAIL ENVELOPE-----") != std::string::npos;
}

} // namespace MailCrypto
