#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <string>
#include "app.hpp"
#include "core/chat_ctrl.hpp"
#include "core/member_mgr.hpp"
#include "net/rx_thread.hpp"
#include "ui/main_window.hpp"
#include "util/log.hpp"
#include "util/text.hpp"

static std::string default_name() {
  wchar_t host[128] = L"";
  DWORD n = 128;
  if (!GetComputerNameW(host, &n))
    swprintf(host, 128, L"PC-%lu", static_cast<unsigned long>(GetCurrentProcessId()));
  return util::wide_to_utf8(host) + "-" + std::to_string(GetCurrentProcessId());
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
  // 用法：mcast-chat.exe [昵称] [--send <文本>]
  // --send 为隐藏参数，仅用于自动化冒烟测试：启动后延时发送一条文本
  int argc = 0;
  std::string name;
  std::wstring autoSend;
  if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
    for (int i = 1; i < argc; ++i) {
      if (wcscmp(argv[i], L"--send") == 0 && i + 1 < argc) {
        autoSend = argv[++i];
      } else if (name.empty()) {
        name = util::wide_to_utf8(argv[i]);
      }
    }
    LocalFree(argv);
  }
  if (name.empty()) name = default_name();

  if (!app().init(name)) {
    MessageBoxW(nullptr, L"初始化身份失败（UUID 生成失败）", L"mcast-chat", MB_ICONERROR);
    return 1;
  }

  std::string err;
  if (!app().sock.start(net::kDefaultPort, net::kDefaultGroup, &err)) {
    MessageBoxW(nullptr, util::utf8_to_wide("加入多播组失败: " + err).c_str(),
                L"mcast-chat", MB_ICONERROR);
    return 1;
  }

  core::members().init_self();

  if (!ui::create_main_window(hInst, nCmdShow)) return 1;
  util::dlog("main: window created, hwnd=%p", (void*)app().hwnd);
  if (!net::start_rx_thread()) {
    MessageBoxW(nullptr, L"接收线程启动失败", L"mcast-chat", MB_ICONERROR);
    return 1;
  }
  util::dlog("main: rx thread started, name=%s", name.c_str());
  core::members().announce();  // 接收线程就绪后再宣告入组，避免丢 ACK

  if (!autoSend.empty()) {
    Sleep(1200);  // 等接收方窗口就绪
    bool ok = core::send_text(util::wide_to_utf8(autoSend));
    util::dlog("main: autosend ok=%d", (int)ok);
  }

  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0) > 0) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  return static_cast<int>(m.wParam);
}
