#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstddef>
#include <cstdint>
#include <string>

namespace net {

constexpr const char* kDefaultGroup = "239.255.42.99";
constexpr uint16_t kDefaultPort = 54321;

// UDP 多播套接字：加组 / 回环 / TTL / 收发。
// send 可在任意线程调用；recv 在接收线程轮询（内部 300ms 超时，保证能响应退出标志）。
class MulticastSocket {
 public:
  MulticastSocket() = default;
  ~MulticastSocket();
  MulticastSocket(const MulticastSocket&) = delete;
  MulticastSocket& operator=(const MulticastSocket&) = delete;

  bool start(uint16_t port = kDefaultPort, const char* group = kDefaultGroup,
             std::string* err = nullptr, const char* bindAddr = nullptr);
  void stop();

  bool send(const uint8_t* data, size_t len, std::string* err = nullptr);
  // 返回收到的字节数；超时返回 0；出错返回 -1
  int recv(uint8_t* buf, size_t bufLen);

 private:
  SOCKET sock_ = INVALID_SOCKET;
  sockaddr_in dst_{};  // 组地址:端口（发送目标）
  bool wsaStarted_ = false;
};

}  // namespace net
