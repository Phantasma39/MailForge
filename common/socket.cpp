// socket.cpp —— TCP Socket 跨平台封装实现（Winsock / POSIX）
#include "common/socket.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>  // POSIX: addrinfo / getaddrinfo / freeaddrinfo / gai_strerror
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "common/logger.hpp"

namespace mail {
namespace {

#if defined(_WIN32)
// Winsock 2.2 全局初始化：首次调用即启动，进程退出自动清理
struct WinsockGuard {
  WinsockGuard() {
    WSADATA d;
    if (WSAStartup(MAKEWORD(2, 2), &d) != 0)
      std::fprintf(stderr, "[socket] WSAStartup 失败\n");
  }
  ~WinsockGuard() { WSACleanup(); }
};
void EnsureSocketInit() { static WinsockGuard g; }

std::string LastErrorText() {
  const DWORD e = WSAGetLastError();
  char* msg = nullptr;
  const DWORD n = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, e, 0, reinterpret_cast<char*>(&msg), 0, nullptr);
  std::string out =
      (n && msg) ? std::string(msg, n)
                 : ("Winsock 错误码 " + std::to_string(e));
  if (msg) LocalFree(msg);
  while (!out.empty() &&
         (out.back() == '\n' || out.back() == '\r' || out.back() == '.'))
    out.pop_back();
  return out;
}
#else
void EnsureSocketInit() {}
std::string LastErrorText() { return std::strerror(errno); }
#endif

constexpr std::intptr_t kInvalidHandle = -1;
bool ValidHandle(std::intptr_t h) { return h != kInvalidHandle; }

#if defined(_WIN32)
using NativeT = SOCKET;
using AddrLenT = int;
#else
using NativeT = int;
using AddrLenT = socklen_t;
#endif
NativeT ToNative(std::intptr_t h) {
#if defined(_WIN32)
  return static_cast<SOCKET>(h);
#else
  return static_cast<int>(h);
#endif
}

// 创建 TCP 套接字；失败返回 kInvalidHandle
std::intptr_t CreateSocket() {
#if defined(_WIN32)
  SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) {
    LOG_ERROR("socket() 创建失败: " << LastErrorText());
    return kInvalidHandle;
  }
  return static_cast<std::intptr_t>(s);
#else
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    LOG_ERROR("socket() 创建失败: " << LastErrorText());
    return kInvalidHandle;
  }
  return static_cast<std::intptr_t>(fd);
#endif
}

bool CloseHandle(std::intptr_t h) {
  if (!ValidHandle(h)) return true;
#if defined(_WIN32)
  if (::closesocket(ToNative(h)) != 0) {
    LOG_WARN("closesocket 失败: " << LastErrorText());
    return false;
  }
#else
  if (::close(ToNative(h)) != 0) {
    LOG_WARN("close 失败: " << LastErrorText());
    return false;
  }
#endif
  return true;
}

bool SetNonBlocking(std::intptr_t h, bool nonblocking) {
#if defined(_WIN32)
  u_long mode = nonblocking ? 1 : 0;
  if (::ioctlsocket(ToNative(h), FIONBIO, &mode) != 0) {
    LOG_ERROR("设置非阻塞模式失败: " << LastErrorText());
    return false;
  }
#else
  const int flags = ::fcntl(ToNative(h), F_GETFL, 0);
  if (flags < 0) {
    LOG_ERROR("fcntl(F_GETFL) 失败: " << LastErrorText());
    return false;
  }
  const int target =
      nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  if (::fcntl(ToNative(h), F_SETFL, target) < 0) {
    LOG_ERROR("fcntl(F_SETFL) 失败: " << LastErrorText());
    return false;
  }
#endif
  return true;
}

// 等待句柄可读(want_write=false)/可写(true)。返回 1=就绪 0=超时 -1=错误
int WaitHandle(std::intptr_t h, int timeout_ms, bool want_write) {
  if (!ValidHandle(h)) return -1;
  for (int attempt = 0; attempt < 3; ++attempt) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(ToNative(h), &fds);
    timeval tv{};
    timeval* p_tv = nullptr;
    if (timeout_ms >= 0) {
      tv.tv_sec = timeout_ms / 1000;
      tv.tv_usec = (timeout_ms % 1000) * 1000;
      p_tv = &tv;
    }
#if defined(_WIN32)
    const int r = ::select(0, want_write ? nullptr : &fds,
                           want_write ? &fds : nullptr, nullptr, p_tv);
#else
    const int r = ::select(ToNative(h) + 1, want_write ? nullptr : &fds,
                           want_write ? &fds : nullptr, nullptr, p_tv);
#endif
    if (r > 0) return 1;
    if (r == 0) return 0;
#if defined(_WIN32)
    if (WSAGetLastError() != WSAEINTR) {
      LOG_ERROR("select 失败: " << LastErrorText());
      return -1;
    }
#else
    if (errno != EINTR) {
      LOG_ERROR("select 失败: " << LastErrorText());
      return -1;
    }
#endif
  }
  return -1;
}

// 单次发送，返回实际字节数；失败返回 -1
std::int32_t SendOnce(std::intptr_t h, const char* data, std::size_t len) {
  for (;;) {
#if defined(_WIN32)
    const int r = ::send(ToNative(h), data, static_cast<int>(len), 0);
    if (r != SOCKET_ERROR) return r;
    if (WSAGetLastError() == WSAEINTR) continue;
#else
    const ssize_t r = ::send(ToNative(h), data, len, 0);
    if (r >= 0) return static_cast<std::int32_t>(r);
    if (errno == EINTR) continue;
#endif
    return -1;
  }
}

// 单次接收（不触碰行缓冲），结果回填 got
RecvStatus RecvOnce(std::intptr_t h, char* buf, std::size_t cap,
                    std::size_t* got) {
  *got = 0;
  for (int attempt = 0; attempt < 3; ++attempt) {
#if defined(_WIN32)
    const int r = ::recv(ToNative(h), buf, static_cast<int>(cap), 0);
    if (r > 0) {
      *got = static_cast<std::size_t>(r);
      return RecvStatus::kOk;
    }
    if (r == 0) return RecvStatus::kClosed;
    const int e = WSAGetLastError();
    if (e == WSAEINTR) continue;
    if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) return RecvStatus::kTimeout;
    LOG_ERROR("recv 失败: " << LastErrorText());
    return RecvStatus::kError;
#else
    const ssize_t r = ::recv(ToNative(h), buf, cap, 0);
    if (r > 0) {
      *got = static_cast<std::size_t>(r);
      return RecvStatus::kOk;
    }
    if (r == 0) return RecvStatus::kClosed;
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return RecvStatus::kTimeout;
    LOG_ERROR("recv 失败: " << LastErrorText());
    return RecvStatus::kError;
#endif
  }
  return RecvStatus::kError;
}

// getaddrinfo 错误文本
std::string GaiErrorText(int code) {
#if defined(_WIN32)
  return gai_strerrorA(code);
#else
  return gai_strerror(code);
#endif
}

// sockaddr_in -> "ip:port"
std::string FormatSockAddr(const sockaddr_in& sa) {
  const auto* b = reinterpret_cast<const unsigned char*>(&sa.sin_addr);
  char ip[16];
  std::snprintf(ip, sizeof ip, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
  return std::string(ip) + ":" + std::to_string(ntohs(sa.sin_port));
}

// 设置 SO_RCVTIMEO / SO_SNDTIMEO
//   注意：Windows 的 SO_RCVTIMEO 只接受 int 毫秒（timeval 的 tv_usec 被按毫秒
//   解释，极易出错）；POSIX 才使用标准 timeval（微秒）。
//   timeout_ms < 0 表示清零（恢复默认阻塞）。
bool SetSocketTimeout(std::intptr_t h, int optname, int timeout_ms) {
  if (!ValidHandle(h)) return false;
#if defined(_WIN32)
  const int ms = (timeout_ms < 0) ? 0 : timeout_ms;
  if (::setsockopt(ToNative(h), SOL_SOCKET, optname,
                   reinterpret_cast<const char*>(&ms), sizeof(ms)) != 0) {
    LOG_ERROR("setsockopt 超时设置失败: " << LastErrorText());
    return false;
  }
#else
  timeval tv{};
  if (timeout_ms > 0) {
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
  }
  if (::setsockopt(ToNative(h), SOL_SOCKET, optname,
                   reinterpret_cast<const char*>(&tv), sizeof(tv)) != 0) {
    LOG_ERROR("setsockopt 超时设置失败: " << LastErrorText());
    return false;
  }
#endif
  return true;
}

// 非阻塞 connect + select 实现的连接尝试（带超时）
// 返回 0=成功；1=失败（err 已填）；2=超时
int ConnectWithTimeout(std::intptr_t h, const addrinfo* ai, int timeout_ms,
                       std::string* err) {
  if (timeout_ms < 0) {
    if (::connect(ToNative(h), ai->ai_addr,
                  static_cast<AddrLenT>(ai->ai_addrlen)) == 0)
      return 0;
    *err = LastErrorText();
    return 1;
  }
  if (!SetNonBlocking(h, true)) {
    *err = LastErrorText();
    return 1;
  }
  const int rc = ::connect(ToNative(h), ai->ai_addr,
                           static_cast<AddrLenT>(ai->ai_addrlen));
  if (rc != 0) {
#if defined(_WIN32)
    const int e = WSAGetLastError();
    if (e != WSAEWOULDBLOCK) {
      SetNonBlocking(h, false);
      *err = LastErrorText();
      return 1;
    }
#else
    const int e = errno;
    if (e != EINPROGRESS) {
      SetNonBlocking(h, false);
      *err = LastErrorText();
      return 1;
    }
#endif
    const int wr = WaitHandle(h, timeout_ms, /*want_write=*/true);
    if (wr == 0) {
      SetNonBlocking(h, false);
      *err = "连接超时（" + std::to_string(timeout_ms) + " ms）";
      return 2;
    }
    if (wr < 0) {
      SetNonBlocking(h, false);
      *err = LastErrorText();
      return 1;
    }
    // 非阻塞 connect 的最终结果须通过 SO_ERROR 查询
    int so_err = 0;
    AddrLenT so_len = sizeof(so_err);
    if (::getsockopt(ToNative(h), SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&so_err), &so_len) != 0) {
      SetNonBlocking(h, false);
      *err = LastErrorText();
      return 1;
    }
    if (so_err != 0) {
#if defined(_WIN32)
      WSASetLastError(so_err);
#else
      errno = so_err;
#endif
      SetNonBlocking(h, false);
      *err = LastErrorText();
      return 1;
    }
  }
  SetNonBlocking(h, false);
  return 0;
}

}  // namespace

std::string SocketErrorString() { return LastErrorText(); }

// ============================ TcpSocket ============================
TcpSocket::TcpSocket() : handle_(kInvalidHandle) {}
TcpSocket::TcpSocket(std::intptr_t handle) : handle_(handle) {}
TcpSocket::~TcpSocket() { Close(); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept
    : handle_(other.handle_), recv_buf_(std::move(other.recv_buf_)) {
  other.handle_ = kInvalidHandle;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
  if (this != &other) {
    Close();
    handle_ = other.handle_;
    recv_buf_ = std::move(other.recv_buf_);
    other.handle_ = kInvalidHandle;
  }
  return *this;
}

TcpSocket TcpSocket::Connect(const std::string& host, std::uint16_t port,
                             int timeout_ms) {
  EnsureSocketInit();
  addrinfo hints{};
  hints.ai_family = AF_INET;  // 本项目收发演示均为 IPv4
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  const std::string port_str = std::to_string(port);

  addrinfo* res = nullptr;
  const int rc = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
  if (rc != 0)
    throw SocketError("解析主机失败 " + host + ":" + port_str + " —— " +
                      GaiErrorText(rc));

  std::string last_err;
  std::intptr_t fd = kInvalidHandle;
  for (const addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
    fd = CreateSocket();
    if (fd == kInvalidHandle) {
      last_err = SocketErrorString();
      continue;
    }
    const int r = ConnectWithTimeout(fd, ai, timeout_ms, &last_err);
    if (r == 0) {
      ::freeaddrinfo(res);
      return TcpSocket(fd);
    }
    CloseHandle(fd);
    fd = kInvalidHandle;
    if (r == 2) break;  // 超时不必再试其余地址
  }
  ::freeaddrinfo(res);
  throw SocketError("连接 " + host + ":" + port_str + " 失败 —— " + last_err);
}

bool TcpSocket::Valid() const { return ValidHandle(handle_); }

void TcpSocket::Close() {
  if (!ValidHandle(handle_)) return;
  CloseHandle(handle_);
  handle_ = kInvalidHandle;
  recv_buf_.clear();
}

bool TcpSocket::SendAll(const void* data, std::size_t len) {
  if (!ValidHandle(handle_)) return false;
  const auto* p = static_cast<const char*>(data);
  std::size_t sent = 0;
  while (sent < len) {
    const std::int32_t n = SendOnce(handle_, p + sent, len - sent);
    if (n <= 0) {
      LOG_ERROR("SendAll 失败（已发送 " << sent << "/" << len << " 字节）: "
                                       << LastErrorText());
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

bool TcpSocket::SendAll(const std::string& data) {
  return data.empty() ? true : SendAll(data.data(), data.size());
}

bool TcpSocket::SendLine(const std::string& line) {
  std::string out = line;
  out += "\r\n";
  return SendAll(out);
}

RecvStatus TcpSocket::Recv(void* buf, std::size_t cap, std::size_t* got) {
  *got = 0;
  if (!ValidHandle(handle_)) return RecvStatus::kError;
  // 优先吐出缓冲区内尚未被 ReadLine 消费的数据
  if (!recv_buf_.empty()) {
    const std::size_t take = (recv_buf_.size() < cap) ? recv_buf_.size() : cap;
    std::memcpy(buf, recv_buf_.data(), take);
    recv_buf_.erase(0, take);
    *got = take;
    return RecvStatus::kOk;
  }
  return RecvOnce(handle_, static_cast<char*>(buf), cap, got);
}

RecvStatus TcpSocket::ReadLine(std::string* out, std::size_t max_line) {
  out->clear();
  if (!ValidHandle(handle_)) return RecvStatus::kError;
  for (;;) {
    // 1) 尝试从缓冲中提取完整行
    const std::string::size_type nl = recv_buf_.find('\n');
    if (nl != std::string::npos) {
      std::string line = recv_buf_.substr(0, nl);
      recv_buf_.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.size() > max_line) {
        LOG_ERROR("ReadLine 行过长（" << line.size() << " > " << max_line
                                      << "），协议解析中止");
        return RecvStatus::kError;
      }
      *out = std::move(line);
      return RecvStatus::kOk;
    }
    // 2) 无换行但缓冲已超限 —— 防内存无限增长
    if (recv_buf_.size() > max_line) {
      LOG_ERROR("ReadLine 缓冲超限且无换行，协议解析中止");
      return RecvStatus::kError;
    }
    // 3) 底层再收一段（不经过 Recv，避免把缓冲数据重复拼接）
    char chunk[4096];
    std::size_t got = 0;
    const RecvStatus st = RecvOnce(handle_, chunk, sizeof chunk, &got);
    if (st == RecvStatus::kOk) {
      recv_buf_.append(chunk, got);
      continue;  // 回到 1) 继续寻找换行
    }
    return st;  // kTimeout / kClosed / kError：缓冲保留，供调用方决定
  }
}

bool TcpSocket::SetRecvTimeout(int timeout_ms) {
  return SetSocketTimeout(handle_, SO_RCVTIMEO, timeout_ms);
}

bool TcpSocket::SetSendTimeout(int timeout_ms) {
  return SetSocketTimeout(handle_, SO_SNDTIMEO, timeout_ms);
}

bool TcpSocket::ShutdownSend() {
  if (!ValidHandle(handle_)) return false;
#if defined(_WIN32)
  if (::shutdown(ToNative(handle_), SD_SEND) != 0) {
#else
  if (::shutdown(ToNative(handle_), SHUT_WR) != 0) {
#endif
    LOG_ERROR("shutdown 失败: " << LastErrorText());
    return false;
  }
  return true;
}

std::string TcpSocket::PeerAddress() const {
  if (!ValidHandle(handle_)) return "";
  sockaddr_in sa{};
  AddrLenT len = sizeof(sa);
  if (::getpeername(ToNative(handle_), reinterpret_cast<sockaddr*>(&sa), &len) != 0)
    return "";
  return FormatSockAddr(sa);
}

// ============================ TcpListener ============================
TcpListener::TcpListener() : handle_(kInvalidHandle) {}
TcpListener::TcpListener(std::intptr_t handle) : handle_(handle) {}
TcpListener::~TcpListener() { Close(); }

TcpListener::TcpListener(TcpListener&& other) noexcept
    : handle_(other.handle_) {
  other.handle_ = kInvalidHandle;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
  if (this != &other) {
    Close();
    handle_ = other.handle_;
    other.handle_ = kInvalidHandle;
  }
  return *this;
}

TcpListener TcpListener::Listen(const std::string& host, std::uint16_t port,
                                int backlog) {
  EnsureSocketInit();
  if (backlog <= 0) backlog = 32;

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;  // host 为空时绑定 INADDR_ANY
  const std::string port_str = std::to_string(port);
  const char* node = host.empty() ? nullptr : host.c_str();

  addrinfo* res = nullptr;
  const int rc = ::getaddrinfo(node, port_str.c_str(), &hints, &res);
  if (rc != 0)
    throw SocketError("监听地址解析失败 " + (host.empty() ? std::string("*") : host) +
                      ":" + port_str + " —— " + GaiErrorText(rc));

  std::string last_err;
  std::intptr_t fd = kInvalidHandle;
  for (const addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
    fd = CreateSocket();
    if (fd == kInvalidHandle) {
      last_err = SocketErrorString();
      continue;
    }
    // 允许快速重启（压力测试反复监听同一端口）
    int reuse = 1;
    ::setsockopt(ToNative(fd), SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    if (::bind(ToNative(fd), ai->ai_addr,
               static_cast<AddrLenT>(ai->ai_addrlen)) == 0 &&
        ::listen(ToNative(fd), backlog) == 0) {
      ::freeaddrinfo(res);
      return TcpListener(fd);
    }
    last_err = LastErrorText();
    LOG_ERROR("bind/listen 失败: " << last_err);
    CloseHandle(fd);
    fd = kInvalidHandle;
  }
  ::freeaddrinfo(res);
  throw SocketError("监听 " + (host.empty() ? std::string("*") : host) + ":" +
                    port_str + " 失败 —— " + last_err);
}

bool TcpListener::Valid() const { return ValidHandle(handle_); }

void TcpListener::Close() {
  if (!ValidHandle(handle_)) return;
  CloseHandle(handle_);
  handle_ = kInvalidHandle;
}

bool TcpListener::Accept(TcpSocket* out, int timeout_ms) {
  out->Close();
  if (!ValidHandle(handle_)) return false;

  if (timeout_ms >= 0) {
    const int r = WaitHandle(handle_, timeout_ms, /*want_write=*/false);
    if (r <= 0) return false;  // 0=超时；-1=错误（内部已记录日志）
  }

  sockaddr_in peer{};
  AddrLenT len = sizeof(peer);
#if defined(_WIN32)
  const SOCKET s =
      ::accept(ToNative(handle_), reinterpret_cast<sockaddr*>(&peer), &len);
  if (s == INVALID_SOCKET) {
    LOG_ERROR("accept 失败: " << LastErrorText());
    return false;
  }
  *out = TcpSocket(static_cast<std::intptr_t>(s));
#else
  const int fd = ::accept(ToNative(handle_), reinterpret_cast<sockaddr*>(&peer),
                          &len);
  if (fd < 0) {
    LOG_ERROR("accept 失败: " << LastErrorText());
    return false;
  }
  *out = TcpSocket(static_cast<std::intptr_t>(fd));
#endif
  return true;
}

std::uint16_t TcpListener::LocalPort() const {
  if (!ValidHandle(handle_)) return 0;
  sockaddr_in sa{};
  AddrLenT len = sizeof(sa);
  if (::getsockname(ToNative(handle_), reinterpret_cast<sockaddr*>(&sa), &len) != 0)
    return 0;
  return ntohs(sa.sin_port);
}

std::string TcpListener::LocalAddress() const {
  if (!ValidHandle(handle_)) return "";
  sockaddr_in sa{};
  AddrLenT len = sizeof(sa);
  if (::getsockname(ToNative(handle_), reinterpret_cast<sockaddr*>(&sa), &len) != 0)
    return "";
  return FormatSockAddr(sa);
}

}  // namespace mail
