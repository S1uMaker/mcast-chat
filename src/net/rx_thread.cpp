#include "rx_thread.hpp"
#include <cstdlib>
#include <cstring>
#include <vector>
#include <windows.h>
#include "../app.hpp"

namespace net {

// 接收线程：阻塞轮询多播报文，整包拷贝到堆上后 PostMessage 投递给 UI 线程。
// 所有 UI 更新都集中在 UI 线程，天然免锁。
static DWORD WINAPI rx_thread_proc(LPVOID) {
  App& a = app();
  std::vector<uint8_t> buf(65536);
  while (!a.rxStop.load()) {
    uint32_t fromIp = 0;
    int n = a.sock.recv(buf.data(), buf.size(), &fromIp);
    if (n <= 0) continue;  // 超时或瞬时错误
    if (n < static_cast<int>(proto::kHeaderSize)) continue;  // 残包直接丢
    RxPacket* pkt = static_cast<RxPacket*>(malloc(sizeof(RxPacket) + static_cast<size_t>(n)));
    if (!pkt) continue;
    pkt->fromIp = fromIp;
    pkt->len = static_cast<uint32_t>(n);
    memcpy(pkt->data, buf.data(), static_cast<size_t>(n));
    if (!PostMessageW(a.hwnd, WM_APP_RX, 0, (LPARAM)pkt)) free(pkt);
  }
  return 0;
}

bool start_rx_thread() {
  App& a = app();
  a.rxStop.store(false);
  a.rxThread = CreateThread(nullptr, 0, rx_thread_proc, nullptr, 0, nullptr);
  return a.rxThread != nullptr;
}

void stop_rx_thread() {
  App& a = app();
  a.rxStop.store(true);
  if (a.rxThread) {
    WaitForSingleObject(a.rxThread, 2000);
    CloseHandle(a.rxThread);
    a.rxThread = nullptr;
  }
}

}  // namespace net
