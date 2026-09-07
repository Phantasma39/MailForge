// base64.hpp —— RFC 4648 标准 Base64 编解码（跨平台）
//
// 用途：
//   1) 加密后的二进制密文必须转成 ASCII 才能通过 SMTP 传输
//   2) 数字信封中 EncKey / IV / Signature / Body 字段均用 Base64 表示
//   3) SMTP 认证（AUTH PLAIN）与邮件正文（MIME）也可能用到
#ifndef MAIL_COMMON_BASE64_HPP_
#define MAIL_COMMON_BASE64_HPP_

#include <cstddef>
#include <string>
#include <vector>

namespace mail {

// ---------------------------------------------------------------------------
// 标准 Base64 编码（RFC 4648）
// 输入任意二进制数据，输出标准 Base64 字符串（含 '=' 填充，无换行）
// ---------------------------------------------------------------------------
std::string Base64Encode(const unsigned char* data, std::size_t len);
std::string Base64Encode(const std::vector<unsigned char>& data);
std::string Base64Encode(const std::string& data);

// ---------------------------------------------------------------------------
// 带换行的 Base64 编码（RFC 2045 MIME 风格）
// 每 width 字符插入一个 '\n'，用于 SMTP 传输长密文（避免超长行）
// ---------------------------------------------------------------------------
std::string Base64EncodeWrapped(const unsigned char* data, std::size_t len,
                                std::size_t width = 76);

// ---------------------------------------------------------------------------
// 标准 Base64 解码
// 容忍空白字符（\n \r 空格 \t）；若输入非法字符则返回空 vector
// ---------------------------------------------------------------------------
std::vector<unsigned char> Base64Decode(const std::string& text);

// 便捷函数：解码后直接转换为 std::string（用于恢复文本型数据）
std::string Base64DecodeToString(const std::string& text);

}  // namespace mail

#endif  // MAIL_COMMON_BASE64_HPP_
