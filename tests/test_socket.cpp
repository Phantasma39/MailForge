// test_socket.cpp —— Socket 跨平台封装单元测试（本机环回）
//
// 验证点：
//   1. 监听随机端口 / 客户端连接 / accept 握手
//   2. SendLine/ReadLine 行协议往返（自动处理 \r\n）
//   3. 一次 TCP 段到达多行、半行跨多次 recv 拼接
//   4. 读超时返回 kTimeout
//   5. 对端关闭返回 kClosed
//   6. 大块原始数据收发一致
//   7. 连接已关闭端口抛出 SocketError
//   8. host 名 "localhost" 解析连接
#include <cstdio>
#include <exception>
#include <string>
#include <thread>

#include "common/socket.hpp"

namespace {

int g_failures = 0;

void Check(bool cond, const char* what) {
  if (!cond) {
    std::printf("[FAIL] %s\n", what);
    ++g_failures;
  } else {
    std::printf("[ OK ] %s\n", what);
  }
}

// 环回连接对：监听器 + 已接受的服务端连接 + 客户端连接
struct TestConn {
  mail::TcpListener listener;
  mail::TcpSocket server;
  mail::TcpSocket client;

  static TestConn Make() {
    TestConn c;
    c.listener = mail::TcpListener::Listen("127.0.0.1", 0);
    const std::uint16_t port = c.listener.LocalPort();
    if (port == 0) throw mail::SocketError("本地端口获取失败");
    c.client = mail::TcpSocket::Connect("127.0.0.1", port, 3000);
    if (!c.listener.Accept(&c.server, 3000))
      throw mail::SocketError("accept 失败");
    return c;
  }
};

void TestBasicEcho() {
  std::printf("--- 基本行协议往返 ---\n");
  try {
    TestConn c = TestConn::Make();
    Check(c.listener.Valid() && c.server.Valid() && c.client.Valid(),
          "监听/accept/连接三方句柄有效");

    Check(c.client.SendLine("EHLO alice.test"), "客户端发送 EHLO 行");
    std::string line;
    Check(c.server.ReadLine(&line) == mail::RecvStatus::kOk &&
              line == "EHLO alice.test",
          "服务端读到 EHLO 行（行尾 \\r\\n 已被去除）");

    Check(c.server.SendLine("250-alice.test"), "服务端发送 250 多行首行");
    Check(c.server.SendLine("250 AUTH PLAIN LOGIN"), "服务端发送 250 末行");
    Check(c.client.ReadLine(&line) == mail::RecvStatus::kOk &&
              line == "250-alice.test",
          "客户端读多行首行");
    Check(c.client.ReadLine(&line) == mail::RecvStatus::kOk &&
              line == "250 AUTH PLAIN LOGIN",
          "客户端读多行末行");
  } catch (const std::exception& e) {
    Check(false, e.what());
  }
}

void TestCoalescedAndSplitLines() {
  std::printf("--- TCP 拆包/粘包场景 ---\n");
  try {
    // 场景 A：一次 SendAll 连发 3 行（粘包），应能读出 3 条
    {
      TestConn c = TestConn::Make();
      Check(c.server.SendAll("A\r\nB\r\nC\r\n"), "一次 SendAll 发送 3 行");
      std::string line;
      const bool ok =
          c.client.ReadLine(&line) == mail::RecvStatus::kOk && line == "A" &&
          c.client.ReadLine(&line) == mail::RecvStatus::kOk && line == "B" &&
          c.client.ReadLine(&line) == mail::RecvStatus::kOk && line == "C";
      Check(ok, "粘包三行被依次完整读出");
    }

    // 场景 B：半行 "he" 先到，等待后补 "llo\r\n"（拆包），应跨 recv 拼成 "hello"
    {
      TestConn c = TestConn::Make();
      Check(c.client.SetRecvTimeout(300), "客户端设置接收超时 300ms");
      Check(c.server.SendAll("he"), "先发送半行 'he'");
      std::string line;
      Check(c.client.ReadLine(&line) == mail::RecvStatus::kTimeout,
            "半行未结束时 ReadLine 返回 kTimeout（数据暂存）");
      Check(c.server.SendAll("llo\r\n"), "补发剩余 'llo\\r\\n'");
      Check(c.client.ReadLine(&line) == mail::RecvStatus::kOk &&
                line == "hello",
            "半行跨多次 recv 拼接为 'hello'");
    }

    // 场景 C：无任何数据时读，应超时
    {
      TestConn c = TestConn::Make();
      Check(c.client.SetRecvTimeout(150), "设置 150ms 读超时");
      std::string line;
      Check(c.client.ReadLine(&line) == mail::RecvStatus::kTimeout,
            "对端静默时读取返回 kTimeout");
    }
  } catch (const std::exception& e) {
    Check(false, e.what());
  }
}

void TestPeerClose() {
  std::printf("--- 对端关闭检测 ---\n");
  try {
    TestConn c = TestConn::Make();
    std::string line;
    c.server.SendLine("250 再见");
    Check(c.client.ReadLine(&line) == mail::RecvStatus::kOk &&
              line == "250 再见",
          "关闭前最后一条消息可读");
    c.server.Close();  // 关闭即发送 FIN
    Check(c.client.ReadLine(&line) == mail::RecvStatus::kClosed,
          "对端关闭后 ReadLine 返回 kClosed");
    Check(!c.server.Valid(), "服务端句柄 Close 后失效");
    // 重复关闭应幂等
    c.server.Close();
    Check(true, "重复 Close 幂等不崩溃");
  } catch (const std::exception& e) {
    Check(false, e.what());
  }
}

void TestManyLines() {
  std::printf("--- 2000 行顺序收发 ---\n");
  try {
    TestConn c = TestConn::Make();
    const int kLines = 2000;
    for (int i = 0; i < kLines; ++i) {
      if (!c.client.SendLine("line-" + std::to_string(i))) {
        Check(false, "第 i 行发送失败");
        return;
      }
    }
    std::string line;
    bool ok = true;
    for (int i = 0; i < kLines; ++i) {
      if (c.server.ReadLine(&line) != mail::RecvStatus::kOk ||
          line != "line-" + std::to_string(i)) {
        ok = false;
        break;
      }
    }
    Check(ok, "2000 行发送/接收顺序与内容一致");
  } catch (const std::exception& e) {
    Check(false, e.what());
  }
}

void TestRawLarge() {
  std::printf("--- 大块原始数据收发（200KB，双线程） ---\n");
  try {
    TestConn c = TestConn::Make();
    const std::size_t kN = 200 * 1024;
    std::string payload;
    payload.reserve(kN);
    for (std::size_t i = 0; i < kN; ++i)
      payload.push_back(static_cast<char>((i * 31 + 7) & 0xFF));

    // 环回测试双端在同一进程内，须分两个线程收发，避免内核缓冲写满死锁
    std::thread sender([&c, &payload] {
      if (!c.client.SendAll(payload))
        std::printf("[FAIL] 大块数据发送失败\n");
    });

    std::string got;
    got.reserve(kN);
    char buf[8192];
    std::size_t n = 0;
    bool ok = true;
    while (got.size() < kN) {
      const mail::RecvStatus st = c.server.Recv(buf, sizeof buf, &n);
      if (st == mail::RecvStatus::kClosed || st == mail::RecvStatus::kError) {
        ok = false;
        break;
      }
      if (n > 0) got.append(buf, n);
    }
    sender.join();
    Check(ok && got == payload, "200KB 原始数据逐字节一致");
  } catch (const std::exception& e) {
    Check(false, e.what());
  }
}

void TestConnectRefused() {
  std::printf("--- 连接已关闭端口 ---\n");
  try {
    std::uint16_t port = 0;
    {
      auto l = mail::TcpListener::Listen("127.0.0.1", 0);
      port = l.LocalPort();
    }  // 监听器析构，端口释放且无服务
    if (port == 0) {
      Check(false, "临时端口获取失败");
      return;
    }
    try {
      auto s = mail::TcpSocket::Connect("127.0.0.1", port, 2000);
      s.Close();
      Check(false, "连接已关闭端口应当抛出 SocketError");
    } catch (const mail::SocketError&) {
      Check(true, "连接已关闭端口抛出 SocketError");
    }
  } catch (const std::exception& e) {
    Check(false, e.what());
  }
}

void TestLocalhostResolution() {
  std::printf("--- localhost 域名解析连接 ---\n");
  try {
    TestConn c;
    c.listener = mail::TcpListener::Listen("127.0.0.1", 0);
    const std::uint16_t port = c.listener.LocalPort();
    c.client = mail::TcpSocket::Connect("localhost", port, 3000);
    if (!c.listener.Accept(&c.server, 3000))
      throw mail::SocketError("accept 失败");
    Check(c.client.Valid() && c.server.Valid(),
          "通过 'localhost' 域名成功建立环回连接");
    Check(!c.client.PeerAddress().empty(), "PeerAddress() 返回对端地址");
    Check(!c.listener.LocalAddress().empty(), "LocalAddress() 返回监听地址");
  } catch (const std::exception& e) {
    Check(false, e.what());
  }
}

}  // namespace

int main() {
  // 关闭 stdout 块缓冲：输出被重定向/落盘时也能逐行实时可见，便于定位卡点
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("===== Socket 单元测试 =====\n");
  TestBasicEcho();
  TestCoalescedAndSplitLines();
  TestPeerClose();
  TestManyLines();
  TestRawLarge();
  TestConnectRefused();
  TestLocalhostResolution();

  if (g_failures == 0) {
    std::printf("===== 全部通过 =====\n");
    return 0;
  }
  std::printf("===== 失败 %d 项 =====\n", g_failures);
  return 1;
}

