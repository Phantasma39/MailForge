// test_logger.cpp —— 日志模块冒烟测试
#include <thread>

#include "common/logger.hpp"

int main() {
  mail::LogSetLevel(mail::LogLevel::kDebug);
  mail::LogSetTag("test");

  LOG_DEBUG("debug 消息，应当显示");
  LOG_INFO("info 消息：1 + 1 = " << (1 + 1));
  LOG_WARN("warn 消息");
  LOG_ERROR("error 消息");

  // 多线程并发写日志，验证线程安全（不崩溃、不交错乱行）
  std::thread t1([] { for (int i = 0; i < 100; ++i) LOG_INFO("线程1 第 " << i << " 行"); });
  std::thread t2([] { for (int i = 0; i < 100; ++i) LOG_INFO("线程2 第 " << i << " 行"); });
  t1.join();
  t2.join();

  LOG_INFO("日志模块冒烟测试结束（请人工确认上方输出无乱行）");
  return 0;
}
