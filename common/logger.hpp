// logger.hpp —— 轻量级线程安全日志（跨平台）
//
// 用法：
//   LOG_INFO("SMTP 客户端已连接");
//   LOG_WARN("对端提前关闭");
//   LOG_ERROR(std::string("写入失败: ") + strerror(errno));
//
// 级别：Debug < Info < Warn < Error，默认 Info
#ifndef MAIL_COMMON_LOGGER_HPP_
#define MAIL_COMMON_LOGGER_HPP_

#include <sstream>
#include <string>

namespace mail {

enum class LogLevel { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

// 设置最低输出级别（低于该级别的日志被丢弃）
void LogSetLevel(LogLevel level);

// 设置日志前缀标签（如 "server" / "smtp-client"）
void LogSetTag(const std::string& tag);

// 输出一条日志。内部带锁，多线程安全。
void LogMessage(LogLevel level, const std::string& message);

// 宏：把任何可被 << 拼接的内容转成字符串再输出
#define LOG_DEBUG(msg)                                    \
  do {                                                    \
    std::ostringstream os_;                               \
    os_ << msg;                                           \
    ::mail::LogMessage(::mail::LogLevel::kDebug, os_.str()); \
  } while (0)

#define LOG_INFO(msg)                                     \
  do {                                                    \
    std::ostringstream os_;                               \
    os_ << msg;                                           \
    ::mail::LogMessage(::mail::LogLevel::kInfo, os_.str()); \
  } while (0)

#define LOG_WARN(msg)                                     \
  do {                                                    \
    std::ostringstream os_;                               \
    os_ << msg;                                           \
    ::mail::LogMessage(::mail::LogLevel::kWarn, os_.str()); \
  } while (0)

#define LOG_ERROR(msg)                                    \
  do {                                                    \
    std::ostringstream os_;                               \
    os_ << msg;                                           \
    ::mail::LogMessage(::mail::LogLevel::kError, os_.str()); \
  } while (0)

}  // namespace mail

#endif  // MAIL_COMMON_LOGGER_HPP_
