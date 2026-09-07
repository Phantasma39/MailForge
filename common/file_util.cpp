// file_util.cpp —— 跨平台文件工具实现
#include "common/file_util.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace mail {

#if defined(_WIN32)
namespace {

// UTF-8 窄字符串 -> UTF-16 宽字符串
std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return L"";
  const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                      &out[0], len);
  return out;
}

// "rb" 等 ASCII 模式串 -> 宽字符串
std::wstring AsciiToWide(const char* s) {
  std::wstring out;
  for (const char* p = s; *p != '\0'; ++p)
    out.push_back(static_cast<wchar_t>(*p));
  return out;
}

}  // namespace
#endif  // _WIN32

FILE* FileOpen(const std::string& path, const char* mode) {
#if defined(_WIN32)
  return _wfopen(Utf8ToWide(path).c_str(), AsciiToWide(mode).c_str());
#else
  return std::fopen(path.c_str(), mode);
#endif
}

bool FileExists(const std::string& path) {
  FILE* fp = FileOpen(path, "rb");
  if (!fp) return false;
  std::fclose(fp);
  return true;
}

}  // namespace mail
