// base64.cpp —— RFC 4648 标准 Base64 编解码实现
#include "common/base64.hpp"

#include <cctype>

namespace mail {
namespace {

// 标准 Base64 字母表（RFC 4648）
constexpr char kBase64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// 将单个 Base64 字符映射为 6-bit 值；非法字符返回 -1
int DecodeChar(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

}  // namespace

std::string Base64Encode(const unsigned char* data, std::size_t len) {
  std::string out;
  out.reserve(((len + 2) / 3) * 4);

  std::size_t i = 0;
  // 每 3 字节 -> 4 个 Base64 字符
  for (; i + 3 <= len; i += 3) {
    unsigned int n = (static_cast<unsigned int>(data[i]) << 16) |
                     (static_cast<unsigned int>(data[i + 1]) << 8) |
                     static_cast<unsigned int>(data[i + 2]);
    out.push_back(kBase64Table[(n >> 18) & 0x3F]);
    out.push_back(kBase64Table[(n >> 12) & 0x3F]);
    out.push_back(kBase64Table[(n >> 6) & 0x3F]);
    out.push_back(kBase64Table[n & 0x3F]);
  }

  // 剩余 1 或 2 字节，按规则补 '='
  const std::size_t remain = len - i;
  if (remain == 1) {
    unsigned int n = static_cast<unsigned int>(data[i]) << 16;
    out.push_back(kBase64Table[(n >> 18) & 0x3F]);
    out.push_back(kBase64Table[(n >> 12) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  } else if (remain == 2) {
    unsigned int n = (static_cast<unsigned int>(data[i]) << 16) |
                     (static_cast<unsigned int>(data[i + 1]) << 8);
    out.push_back(kBase64Table[(n >> 18) & 0x3F]);
    out.push_back(kBase64Table[(n >> 12) & 0x3F]);
    out.push_back(kBase64Table[(n >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

std::string Base64Encode(const std::vector<unsigned char>& data) {
  return data.empty() ? std::string() : Base64Encode(data.data(), data.size());
}

std::string Base64Encode(const std::string& data) {
  return Base64Encode(
      reinterpret_cast<const unsigned char*>(data.data()), data.size());
}

std::string Base64EncodeWrapped(const unsigned char* data, std::size_t len,
                                std::size_t width) {
  if (width == 0) width = 76;  // 非法宽度回退到默认值
  const std::string plain = Base64Encode(data, len);
  if (plain.size() <= width) return plain;

  std::string out;
  out.reserve(plain.size() + plain.size() / width);
  std::size_t pos = 0;
  while (pos < plain.size()) {
    if (!out.empty()) out.push_back('\n');
    const std::size_t take = (plain.size() - pos < width) ? (plain.size() - pos) : width;
    out.append(plain, pos, take);
    pos += take;
  }
  return out;
}

std::vector<unsigned char> Base64Decode(const std::string& text) {
  std::vector<unsigned char> out;
  out.reserve(text.size() * 3 / 4 + 3);

  int buf = 0;   // 已积累的 6-bit 值
  int bits = 0;  // 当前积累的位数
  bool seen_pad = false;

  for (char c : text) {
    if (c == '=') {
      seen_pad = true;  // 填充符（正确性由末尾位数检查把关）
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c))) continue;  // 容忍换行/空格
    if (seen_pad) return {};  // '=' 之后不允许再出现有效字符

    const int v = DecodeChar(c);
    if (v < 0) return {};  // 非法字符

    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<unsigned char>((buf >> bits) & 0xFF));
    }
  }

  // 解码结束时剩余位数只能是 0（恰好整字节）、2（对应一个 '='）、4（对应两个 '='）
  if (bits != 0 && bits != 2 && bits != 4) return {};
  return out;
}

std::string Base64DecodeToString(const std::string& text) {
  const auto bytes = Base64Decode(text);
  return std::string(bytes.begin(), bytes.end());
}

}  // namespace mail
