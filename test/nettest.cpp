// 网络层自测：同进程开两个多播 socket，验证同机多实例的组播分发与回环
// 用法: nettest.exe [--bind-group]
//   默认两个 socket 都绑 INADDR_ANY:port；--bind-group 时绑组地址（Windows 备选方案）
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "../../mcast-chat/src/net/multicast.hpp"
#include "../../mcast-chat/src/protocol/codec.hpp"
#include "../../mcast-chat/src/protocol/message.hpp"

using namespace net;
using namespace proto;

int main(int argc, char** argv) {
  bool bindGroup = argc > 1 && std::strcmp(argv[1], "--bind-group") == 0;
  const char* bindAddr = bindGroup ? kDefaultGroup : nullptr;

  MulticastSocket a, b;
  std::string err;
  if (!a.start(kDefaultPort, kDefaultGroup, &err, bindAddr)) {
    printf("A start FAIL: %s\n", err.c_str());
    return 1;
  }
  if (!b.start(kDefaultPort, kDefaultGroup, &err, bindAddr)) {
    printf("B start FAIL: %s\n", err.c_str());
    return 1;
  }
  printf("both sockets joined %s:%u (bind=%s)\n", kDefaultGroup,
         (unsigned)kDefaultPort, bindGroup ? "group" : "INADDR_ANY");

  std::atomic<int> aGot{0}, bGot{0};
  std::thread ta([&] {
    uint8_t buf[2048];
    ULONGLONG end = GetTickCount64() + 4000;
    while (GetTickCount64() < end)
      if (a.recv(buf, sizeof buf) > 0) aGot.fetch_add(1);
  });
  std::thread tb([&] {
    uint8_t buf[2048];
    ULONGLONG end = GetTickCount64() + 4000;
    while (GetTickCount64() < end)
      if (b.recv(buf, sizeof buf) > 0) bGot.fetch_add(1);
  });

  // B 连发 10 个包（回环开启）：正常情况下 A、B 各自都应收到全部 10 个
  for (int i = 0; i < 10; ++i) {
    Header h;
    h.type = MSG_CHAT_TEXT;
    h.sender.fill(0x42);
    h.seq = static_cast<uint32_t>(i);
    std::vector<uint8_t> payload = {'p', 'i', 'n', 'g'};
    auto pkt = encode_packet(h, payload);
    if (!b.send(pkt.data(), pkt.size(), &err))
      printf("send fail: %s\n", err.c_str());
    Sleep(50);
  }

  ta.join();
  tb.join();
  printf("A received=%d  B(self-loop) received=%d\n", aGot.load(), bGot.load());
  bool ok = aGot.load() >= 5 && bGot.load() >= 5;  // 两个 socket 都拿到一半以上 = 分发正常
  printf("%s\n", ok ? "NETTEST PASS" : "NETTEST FAIL");
  return ok ? 0 : 1;
}
