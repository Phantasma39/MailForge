// 邮件加密模块实现：Base64 + XOR（可用示例），以及可插拔的加/解密入口

#include "MailCrypto.h"

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

        // 【TODO】后续实现：
        // case ALGO_RC4:
        //     return std::string(kEncMagicRc4) + base64Encode(rc4Cipher(plainText, key));
        // case ALGO_AES_CBC:
        //     return std::string(kEncMagicAes) + base64Encode(aesCbcEncrypt(plainText, key));

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

    // 【TODO】按签名头区分 RC4 / AES 的分支加在这里
}

} // namespace MailCrypto
