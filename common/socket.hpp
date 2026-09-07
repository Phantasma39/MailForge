// socket.hpp —— TCP Socket 跨平台封装（Winsock / POSIX）
//
// 定位：SMTP / POP3 手写协议的地基，屏蔽 Windows(Winsock) 与 Linux(POSIX) 差异。
//
// 提供：
//   1) TcpSocket   —— 面向连接的 TCP 套接字（RAII、可移动、带行缓冲读取）
//   2) TcpListener —— 服务端监听器（SO_REUSEADDR、可选超时 accept、随机端口）
//
// 行缓冲说明：SMTP/POP3 是"行"协议，但 TCP 数据可能被拆成任意片段。
//   ReadLine() 内部自带缓冲，能跨多次 recv 拼出完整行（自动去掉行尾 \r\n），
//   也可与 Recv() 混用（Recv() 会优先吐出缓冲区里已有的数据）。
//
// 超时约定：timeout_ms < 0 表示阻塞等待；= 0 立即返回；> 0 为毫秒。
//   读超时返回 RecvStatus::kTimeout；对端关闭返回 kClosed。
#ifndef MAIL_COMMON_SOCKET_HPP_
#define MAIL_COMMON_SOCKET_HPP_

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace mail {

// 读操作结果
enum class RecvStatus {
  kOk,      // 读到数据（可能少于请求长度）
  kClosed,  // 对端已关闭（recv 返回 0）
  kTimeout, // 超时（设置了接收超时后触发）
  kError    // 其它错误
};

// Socket 层致命错误（连接失败 / 监听失败等），携带可读描述
class SocketError : public std::runtime_error {
 public:
  explicit SocketError(const std::string& message)
      : std::runtime_error(message) {}
};

// 最近一次 socket 错误的可读描述（errno / WSAGetLastError，线程相关）
std::string SocketErrorString();

class TcpSocket {
 public:
  TcpSocket();                          // 无效句柄
  ~TcpSocket();
  TcpSocket(const TcpSocket&) = delete;
  TcpSocket& operator=(const TcpSocket&) = delete;
  TcpSocket(TcpSocket&& other) noexcept;
  TcpSocket& operator=(TcpSocket&& other) noexcept;

  // 连接远程主机（host 支持 "127.0.0.1" / "localhost" / 域名，仅 IPv4）。
  // timeout_ms < 0 阻塞直到内核判定失败；否则最多等待该毫秒数。
  // 失败抛 SocketError。
  static TcpSocket Connect(const std::string& host, std::uint16_t port,
                           int timeout_ms = 10000);

  bool Valid() const;
  void Close();                            // 关闭连接（幂等）

  // 发送完整数据（内部处理部分发送与信号中断）
  bool SendAll(const void* data, std::size_t len);
  bool SendAll(const std::string& data);
  bool SendLine(const std::string& line);  // 追加 \r\n 后发送

  // 单次读取，实际字节数回填到 got；0 表示对端已关闭。
  RecvStatus Recv(void* buf, std::size_t cap, std::size_t* got);
  // 读一行（去除 \r\n）。行超长(max_line)返回 kError。
  // 数据可跨多次 recv 到达，ReadLine 自动拼接。
  RecvStatus ReadLine(std::string* out, std::size_t max_line = 8192);
  void DiscardBuffer() { recv_buf_.clear(); }  // 丢弃未消费的缓冲数据

  bool SetRecvTimeout(int timeout_ms);     // <0 恢复默认阻塞
  bool SetSendTimeout(int timeout_ms);
  bool ShutdownSend();                     // 半关闭：发送 FIN，仍可读

  std::string PeerAddress() const;         // "ip:port"
  std::intptr_t NativeHandle() const { return handle_; }

 private:
  friend class TcpListener;
  explicit TcpSocket(std::intptr_t handle);

  std::intptr_t handle_;
  std::string recv_buf_;  // 行缓冲（ReadLine 专用，跨 recv 保留）
};

class TcpListener {
 public:
  TcpListener();                           // 无效句柄
  ~TcpListener();
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;
  TcpListener(TcpListener&& other) noexcept;
  TcpListener& operator=(TcpListener&& other) noexcept;

  // host 为空表示 INADDR_ANY；port = 0 表示随机端口（测试用）。
  // 失败抛 SocketError。
  static TcpListener Listen(const std::string& host, std::uint16_t port,
                            int backlog = 32);

  bool Valid() const;
  void Close();

  // 接受一个连接。timeout_ms < 0 阻塞；否则最多等待该毫秒数。
  // 成功返回 true 并填充 out；超时/失败返回 false。
  bool Accept(TcpSocket* out, int timeout_ms = -1);

  std::uint16_t LocalPort() const;         // 监听端口（0 = 无效）
  std::string LocalAddress() const;        // "ip:port"

 private:
  explicit TcpListener(std::intptr_t handle);
  std::intptr_t handle_;
};

}  // namespace mail

#endif  // MAIL_COMMON_SOCKET_HPP_
