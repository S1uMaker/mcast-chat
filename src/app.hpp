#pragma once
#include <atomic>
#include <string>
#include "net/multicast.hpp"  // 先于 windows.h，保证 winsock2 头先入
#include <windows.h>
#include "protocol/message.hpp"

// UI 线程自定义消息：接收线程把整包拷到堆上后 Post 过来
// wParam = 报文长度, lParam = 堆指针（UI 处理后须 free）
#define WM_APP_RX (WM_APP + 1)

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
