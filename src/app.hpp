#pragma once
#include <atomic>
#include <string>
#include "net/multicast.hpp"  // 先于 windows.h，保证 winsock2 头先入
#include <windows.h>
#include "protocol/message.hpp"

// UI 线程自定义消息
// WM_APP_RX:        lParam = RxPacket*（接收线程堆分配，UI 处理后 free）
// WM_APP_KICK_ROW:  wParam = 成员列表行号（自动化测试钩子，走与右键菜单相同的踢人流程）
#define WM_APP_RX (WM_APP + 1)
#define WM_APP_KICK_ROW (WM_APP + 2)

// 接收线程 → UI 线程的报文载体（malloc 时按 sizeof(RxPacket) + len 分配）
struct RxPacket {
  uint32_t fromIp = 0;  // 源 IP（网络字节序）
  uint32_t len = 0;     // 报文长度
  uint8_t data[1];      // 实际可用长度为 len
};

struct App {
  std::string name;              // UTF-8 昵称
  proto::Uuid uuid{};            // 成员唯一标识
  std::atomic<uint32_t> seq{0};  // 发送序号（单调递增）

  net::MulticastSocket sock;
  HWND hwnd = nullptr;           // 主窗口句柄（接收线程投递目标）
  HANDLE rxThread = nullptr;
  std::atomic<bool> rxStop{false};
  std::atomic<long> rxCount{0};  // 收到的聊天条数（状态栏显示）

  bool init(const std::string& userName);  // 生成 UUID 等身份信息
};

App& app();  // 全局单例
