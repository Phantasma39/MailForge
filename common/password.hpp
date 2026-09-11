#ifndef MAILFORGE_PASSWORD_HPP_
#define MAILFORGE_PASSWORD_HPP_

// ============================================================================
//  password.hpp —— 密码哈希与校验（PBKDF2-HMAC-SHA256）
//
//  存储格式：
//    pbkdf2-sha256$<iterations>$<saltHex>$<hashHex>
//
//  说明：
//    1) 注册 / 修改密码时只写入哈希，不在 users.txt 中保存明文。
//    2) 旧版明文 users.txt 仍可校验；Pop3Server 启动时会自动迁移成哈希格式。
//    3) 校验使用 CRYPTO_memcmp，避免简单字符串比较的时序泄露。
// ============================================================================

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

namespace mail {

inline std::string ToHex(const unsigned char* data, std::size_t len) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out[2 * i]     = kHex[(data[i] >> 4) & 0x0F];
        out[2 * i + 1] = kHex[data[i] & 0x0F];
    }
    return out;
}

inline bool FromHex(const std::string& hex, std::vector<unsigned char>& out) {
    if (hex.size() % 2 != 0) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        int hi = val(hex[i]);
        int lo = val(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<unsigned char>((hi << 4) | lo));
    }
    return true;
}

inline bool ConstantTimeEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

// 生成随机盐（默认 16 字节）
inline bool RandomSalt(std::vector<unsigned char>& salt, std::size_t len = 16) {
    salt.resize(len);
    if (len == 0) return true;
    return RAND_bytes(salt.data(), static_cast<int>(salt.size())) == 1;
}

// 生成密码哈希：pbkdf2-sha256$100000$salthex$hashhex
inline std::string HashPassword(const std::string& password,
                                int iterations = 100000,
                                std::size_t saltLen = 16,
                                std::size_t hashLen = 32) {
    if (iterations <= 0) iterations = 100000;
    std::vector<unsigned char> salt;
    if (!RandomSalt(salt, saltLen)) return "";

    std::vector<unsigned char> hash(hashLen);
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                          salt.data(), static_cast<int>(salt.size()),
                          iterations, EVP_sha256(),
                          static_cast<int>(hash.size()), hash.data()) != 1) {
        return "";
    }

    return "pbkdf2-sha256$" + std::to_string(iterations) + "$" +
           ToHex(salt.data(), salt.size()) + "$" +
           ToHex(hash.data(), hash.size());
}

inline bool IsHashedPassword(const std::string& stored) {
    return stored.rfind("pbkdf2-sha256$", 0) == 0;
}

// 校验密码。stored 既支持新格式，也兼容旧版明文密码。
inline bool VerifyPassword(const std::string& password, const std::string& stored) {
    if (!IsHashedPassword(stored)) {
        // 旧版明文账号：仍然允许登录，便于平滑迁移。
        return ConstantTimeEqual(password, stored);
    }

    const std::string prefix = "pbkdf2-sha256$";
    std::size_t p1 = stored.find('$', prefix.size());
    if (p1 == std::string::npos) return false;
    std::size_t p2 = stored.find('$', p1 + 1);
    if (p2 == std::string::npos) return false;

    const std::string iterStr = stored.substr(prefix.size(), p1 - prefix.size());
    const std::string saltHex = stored.substr(p1 + 1, p2 - p1 - 1);
    const std::string hashHex = stored.substr(p2 + 1);

    char* end = nullptr;
    long iterations = std::strtol(iterStr.c_str(), &end, 10);
    if (end == iterStr.c_str() || iterations <= 0 || iterations > 10000000) {
        return false;
    }

    std::vector<unsigned char> salt;
    std::vector<unsigned char> expected;
    if (!FromHex(saltHex, salt) || !FromHex(hashHex, expected)) return false;
    if (salt.empty() || expected.empty()) return false;

    std::vector<unsigned char> actual(expected.size());
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                          salt.data(), static_cast<int>(salt.size()),
                          static_cast<int>(iterations), EVP_sha256(),
                          static_cast<int>(actual.size()), actual.data()) != 1) {
        return false;
    }

    return ConstantTimeEqual(
        std::string(reinterpret_cast<const char*>(actual.data()), actual.size()),
        std::string(reinterpret_cast<const char*>(expected.data()), expected.size()));
}

} // namespace mail

#endif // MAILFORGE_PASSWORD_HPP_