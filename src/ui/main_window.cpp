#include "main_window.hpp"
#include <commctrl.h>
#include <cstdlib>
#include <cwchar>
#include "../app.hpp"
#include "../core/chat_ctrl.hpp"
#include "../net/rx_thread.hpp"
#include "../protocol/codec.hpp"
#include "../protocol/message.hpp"
#include "../util/log.hpp"
#include "../util/text.hpp"

namespace ui {
namespace {

constexpr int IDC_CHAT    = 101;
constexpr int IDC_MEMBERS = 102;
constexpr int IDC_INPUT   = 103;
constexpr int IDC_SEND    = 104;
constexpr int IDC_STATUS  = 105;

HWND g_chat = nullptr;
HWND g_members = nullptr;
HWND g_input = nullptr;
HWND g_send = nullptr;
HWND g_status = nullptr;
HFONT g_font = nullptr;
WNDPROC g_inputProc = nullptr;  // 输入框原窗口过程（手动子类化：回车即发送）

std::wstring time_prefix() {
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t b[32];
  swprintf(b, 32, L"[%02d:%02d:%02d] ", static_cast<int>(t.wHour),
           static_cast<int>(t.wMinute), static_cast<int>(t.wSecond));
  return b;
}

std::wstring trim(const std::wstring& s) {
  size_t b = s.find_first_not_of(L" \t\r\n");
  if (b == std::wstring::npos) return L"";
  size_t e = s.find_last_not_of(L" \t\r\n");
  return s.substr(b, e - b + 1);
}

void append_chat_line(const std::wstring& line) {
  // ES_READONLY 会拦截 EM_REPLACESEL，追加前临时解除只读
  SendMessageW(g_chat, EM_SETREADONLY, FALSE, 0);
  int len = GetWindowTextLengthW(g_chat);
  SendMessageW(g_chat, EM_SETSEL, len, len);
  std::wstring text = line + L"\r\n";
  SendMessageW(g_chat, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
  SendMessageW(g_chat, EM_SCROLLCARET, 0, 0);
  SendMessageW(g_chat, EM_SETREADONLY, TRUE, 0);
}

void update_status() {
  App& a = app();
  std::wstring s = L"已加入 " + util::utf8_to_wide(net::kDefaultGroup) + L":" +
                   std::to_wstring(static_cast<int>(net::kDefaultPort)) +
                   L"   昵称: " + util::utf8_to_wide(a.name) +
                   L"   收到 " + std::to_wstring(a.rxCount.load()) + L" 条聊天";
  LRESULT r = SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)s.c_str());
  util::dlog("update_status: SB_SETTEXTW -> %td", r);
}

void on_send() {
  wchar_t buf[4096];
  int n = GetWindowTextW(g_input, buf, 4096);
  util::dlog("on_send: enter, text_len=%d", n);
  std::wstring text = trim(std::wstring(buf, static_cast<size_t>(n)));
  if (text.empty()) return;

  std::string err;
  if (core::send_text(util::wide_to_utf8(text), &err)) {
    util::dlog("on_send: send ok");
    append_chat_line(time_prefix() + L"我: " + text);  // 本地回显；回环包按 UUID 丢弃
    util::dlog("on_send: echo appended, chat_len=%d", GetWindowTextLengthW(g_chat));
    SetWindowTextW(g_input, L"");
  } else {
    util::dlog("on_send: send FAILED: %s", err.c_str());
    MessageBoxW(app().hwnd, util::utf8_to_wide("发送失败: " + err).c_str(), L"错误", MB_ICONERROR);
  }
}

// 接收线程投递上来的报文（data 为堆指针，本函数负责释放）
void handle_rx(size_t len, uint8_t* data) {
  App& a = app();
  proto::Packet pkt;
  bool parsed = proto::parse_packet(data, len, pkt);
  util::dlog("rx-ui: len=%zu parsed=%d", len, (int)parsed);
  if (parsed) {
    if (pkt.hdr.type == proto::MSG_CHAT_TEXT && pkt.hdr.sender != a.uuid) {
      proto::Reader r(pkt.payload, pkt.payload_len);
      std::string name, text;
      if (r.utf8(name) && r.utf8(text) && r.ok()) {
        a.rxCount.fetch_add(1);
        util::dlog("rx-ui: text from=%s", name.c_str());
        append_chat_line(time_prefix() + util::utf8_to_wide(name) + L": " +
                         util::utf8_to_wide(text));
        util::dlog("rx-ui: appended, chat_len=%d", GetWindowTextLengthW(g_chat));
        update_status();
      } else {
        util::dlog("rx-ui: TEXT payload parse failed");
      }
    }
    // 其余类型（HELLO / FILE_* / KICK…）由 M2+ 的对应模块接管
  }
  free(data);
}

LRESULT CALLBACK input_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_KEYDOWN && w == VK_RETURN) {
    on_send();
    return 0;
  }
  return CallWindowProcW(g_inputProc, h, m, w, l);
}

void create_controls(HWND hwnd, HINSTANCE hInst) {
  g_chat = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
      0, 0, 0, 0, hwnd, (HMENU)IDC_CHAT, hInst, nullptr);

  g_members = CreateWindowExW(0, WC_LISTVIEWW, L"",
      WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT,
      0, 0, 0, 0, hwnd, (HMENU)IDC_MEMBERS, hInst, nullptr);
  ListView_SetExtendedListViewStyle(g_members, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
  LVCOLUMNW col{};
  col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
  col.pszText = (LPWSTR)L"成员"; col.cx = 110; col.iSubItem = 0;
  ListView_InsertColumn(g_members, 0, &col);
  col.pszText = (LPWSTR)L"地址"; col.cx = 120; col.iSubItem = 1;
  ListView_InsertColumn(g_members, 1, &col);
  col.pszText = (LPWSTR)L"状态"; col.cx = 60; col.iSubItem = 2;
  ListView_InsertColumn(g_members, 2, &col);
  // M1：成员列表先只放自己；M2 由 MemberManager（HELLO/心跳）维护
  std::wstring myName = util::utf8_to_wide(app().name);
  LVITEMW it{};
  it.mask = LVIF_TEXT;
  it.iItem = 0;
  it.pszText = (LPWSTR)myName.c_str();
  ListView_InsertItem(g_members, &it);
  ListView_SetItemText(g_members, 0, 1, (LPWSTR)L"(本机)");
  ListView_SetItemText(g_members, 0, 2, (LPWSTR)L"在线");

  g_input = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, (HMENU)IDC_INPUT, hInst, nullptr);
  g_send = CreateWindowExW(0, L"BUTTON", L"发送",
      WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd, (HMENU)IDC_SEND, hInst, nullptr);
  g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE,
      0, 0, 0, 0, hwnd, (HMENU)IDC_STATUS, hInst, nullptr);

  g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
  HWND ctrls[] = {g_chat, g_members, g_input, g_send, g_status};
  for (HWND h : ctrls) SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
}

void layout(HWND hwnd) {
  if (!g_chat || !g_members || !g_input || !g_send || !g_status) return;
  RECT rc;
  GetClientRect(hwnd, &rc);
  const int pad = 8, listW = 280, btnW = 96, inputH = 30;
  SendMessageW(g_status, WM_SIZE, 0, 0);  // 状态栏自适应宽度
  RECT sr;
  GetWindowRect(g_status, &sr);
  int statusH = sr.bottom - sr.top;
  int bottom = rc.bottom - statusH - pad;
  int topH = bottom - pad - inputH - pad;  // 聊天区/成员列表的高度
  int chatW = rc.right - listW - pad * 3;

  MoveWindow(g_chat, pad, pad, chatW, topH - pad, TRUE);
  MoveWindow(g_members, pad * 2 + chatW, pad, listW, topH - pad, TRUE);
  int by = bottom - inputH;
  MoveWindow(g_input, pad, by, chatW, inputH, TRUE);
  MoveWindow(g_send, pad * 2 + chatW, by, btnW, inputH, TRUE);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
  switch (msg) {
    case WM_CREATE: {
      INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
      InitCommonControlsEx(&icc);
      app().hwnd = hwnd;
      create_controls(hwnd, ((LPCREATESTRUCTW)l)->hInstance);
      g_inputProc = (WNDPROC)SetWindowLongPtrW(g_input, GWLP_WNDPROC, (LONG_PTR)input_proc);
      SetFocus(g_input);
      update_status();
      return 0;
    }
    case WM_SIZE:
      if (w != SIZE_MINIMIZED) layout(hwnd);
      return 0;
    case WM_SETFOCUS:
      SetFocus(g_input);
      return 0;
    case WM_APP_RX:
      handle_rx(static_cast<size_t>(w), (uint8_t*)l);
      return 0;
    case WM_COMMAND:
      if (LOWORD(w) == IDC_SEND && HIWORD(w) == BN_CLICKED) {
        util::dlog("wm_command: SEND clicked");
        on_send();
      }
      return 0;
    case WM_DESTROY: {
      net::stop_rx_thread();
      app().sock.stop();
      // 清掉消息队列里尚未处理的 WM_APP_RX，避免退出时泄漏
      MSG m;
      while (PeekMessageW(&m, hwnd, WM_APP_RX, WM_APP_RX, PM_REMOVE)) free((void*)m.lParam);
      if (g_font) { DeleteObject(g_font); g_font = nullptr; }
      PostQuitMessage(0);
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, w, l);
}

}  // namespace

bool create_main_window(HINSTANCE hInst, int nCmdShow) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = hInst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wc.hIconSm = wc.hIcon;
  wc.lpszClassName = L"McastChatWnd";
  if (!RegisterClassExW(&wc)) return false;

  std::wstring title = L"多播聊天 - " + util::utf8_to_wide(app().name);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1000, 640,
                              nullptr, nullptr, hInst, nullptr);
  if (!hwnd) return false;
  ShowWindow(hwnd, nCmdShow);
  UpdateWindow(hwnd);
  return true;
}

}  // namespace ui
