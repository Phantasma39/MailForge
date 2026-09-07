// logger.cpp —— 轻量级线程安全日志实现
#include "common/logger.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace mail {

namespace {
std::mutex g_log_mutex;
LogLevel g_level = LogLevel::kInfo;
std::string g_tag = "mail";

const char* LevelName(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug: return "DEBUG";
    case LogLevel::kInfo:  return "INFO";
    case LogLevel::kWarn:  return "WARN";
    case LogLevel::kError: return "ERROR";
  }
  return "?";
}
}  // namespace

void LogSetLevel(LogLevel level) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  g_level = level;
}

void LogSetTag(const std::string& tag) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  g_tag = tag;
}

void LogMessage(LogLevel level, const std::string& message) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (static_cast<int>(level) < static_cast<int>(g_level)) return;

  // 本地时间戳 [YYYY-MM-DD HH:MM:SS]
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm_buf{};
#if defined(_WIN32)
  localtime_s(&tm_buf, &t);
#else
  localtime_r(&t, &tm_buf);
#endif
  char ts[32];
  std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

  std::fprintf(stderr, "[%s] [%s] [%s] %s\n",
               ts, LevelName(level), g_tag.c_str(), message.c_str());
}

}  // namespace mail
