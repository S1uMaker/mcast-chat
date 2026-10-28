#include "multicast.hpp"

namespace net {

MulticastSocket::~MulticastSocket() { stop(); }

bool MulticastSocket::start(uint16_t port, const char* group, std::string* err,
                            const char* bindAddr) {
  auto fail = [&](const char* what, int code) {
    if (err) *err = std::string(what) + " (WSA错误码 " + std::to_string(code) + ")";
    return false;
  };

  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return fail("WSAStartup", WSAGetLastError());
  wsaStarted_ = true;

  sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock_ == INVALID_SOCKET) return fail("创建套接字", WSAGetLastError());

  BOOL reuse = TRUE;  // 允许同机多实例绑定同一端口 —— 单机演示 1-N 的关键之一
  setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

  int rcvbuf = 4 * 1024 * 1024;  // 吸收文件分块的突发，减少内核层丢包
  setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf, sizeof(rcvbuf));

  DWORD timeoutMs = 300;  // recv 轮询周期
  setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(port);
  // bindAddr 为空绑 INADDR_ANY；某些 Windows 环境同机多实例需绑组地址才能各自收到
  if (!bindAddr || inet_pton(AF_INET, bindAddr, &local.sin_addr) != 1)
    local.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(sock_, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR)
    return fail("绑定端口", WSAGetLastError());

  dst_.sin_family = AF_INET;
  dst_.sin_port = htons(port);
  if (inet_pton(AF_INET, group, &dst_.sin_addr) != 1) return fail("组地址无效", 0);

  ip_mreq mreq{};
  mreq.imr_multiaddr = dst_.sin_addr;
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  if (setsockopt(sock_, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof(mreq)) == SOCKET_ERROR)
    return fail("加入多播组", WSAGetLastError());

  u_char ttl = 1;  // 同网段；跨路由演示时可调大
  setsockopt(sock_, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));

  BOOL loop = TRUE;  // 本机回环：单机开多实例演示 1-N 的关键之二
  setsockopt(sock_, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof(loop));

  return true;
}

void MulticastSocket::stop() {
  if (sock_ != INVALID_SOCKET) {
    closesocket(sock_);
    sock_ = INVALID_SOCKET;
  }
  if (wsaStarted_) {
    WSACleanup();
    wsaStarted_ = false;
  }
}

bool MulticastSocket::send(const uint8_t* data, size_t len, std::string* err) {
  if (sock_ == INVALID_SOCKET) {
    if (err) *err = "套接字未初始化";
    return false;
  }
  int rc = sendto(sock_, (const char*)data, static_cast<int>(len), 0,
                  (const sockaddr*)&dst_, sizeof(dst_));
  if (rc == SOCKET_ERROR) {
    if (err) *err = "sendto 失败 (WSA错误码 " + std::to_string(WSAGetLastError()) + ")";
    return false;
  }
  return true;
}

int MulticastSocket::recv(uint8_t* buf, size_t bufLen) {
  sockaddr_in from{};
  int fromLen = sizeof(from);
  int n = recvfrom(sock_, (char*)buf, static_cast<int>(bufLen), 0, (sockaddr*)&from, &fromLen);
  if (n == SOCKET_ERROR) {
    int e = WSAGetLastError();
    if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) return 0;
    return -1;
  }
  return n;
}

}  // namespace net
