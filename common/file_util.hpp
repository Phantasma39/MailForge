// file_util.hpp —— 跨平台文件工具（兼容中文路径）
//
// 背景：Windows 下 std::fopen 使用 ANSI 编码，遇到中文路径会失败。
//       本模块用宽字符 API 处理，保证"计算机网络作业"这类目录可正常读写。
#ifndef MAIL_COMMON_FILE_UTIL_HPP_
#define MAIL_COMMON_FILE_UTIL_HPP_

#include <cstdio>
#include <string>

namespace mail {

// 跨平台打开文件（mode: "rb"/"wb"/"ab" 等，与 fopen 一致）
// Windows 下 path 按 UTF-8 解释；失败返回 nullptr
FILE* FileOpen(const std::string& path, const char* mode);

// 文件是否存在
bool FileExists(const std::string& path);

}  // namespace mail

#endif  // MAIL_COMMON_FILE_UTIL_HPP_
