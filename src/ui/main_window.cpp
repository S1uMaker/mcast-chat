#include "main_window.hpp"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>
#include <cstdlib>
#include <cwchar>
#include <vector>
#include "../app.hpp"
#include "../core/chat_ctrl.hpp"
#include "../core/file_xfer.hpp"
#include "../core/member_mgr.hpp"
#include "../net/rx_thread.hpp"
#include "../protocol/codec.hpp"
#include "../protocol/message.hpp"
#include "../util/log.hpp"
#include "../util/text.hpp"

namespace ui {
namespace {

constexpr int IDC_CHAT     = 101;
constexpr int IDC_MEMBERS  = 102;
constexpr int IDC_INPUT    = 103;
constexpr int IDC_SEND     = 104;
constexpr int IDC_STATUS   = 105;
constexpr int IDC_SENDFILE = 106;
constexpr int IDC_PLAY     = 107;
constexpr int IDC_PROGRESS = 108;
constexpr int IDM_KICK     = 40001;
constexpr UINT_PTR TIMER_TICK = 1;

HWND g_chat = nullptr;
HWND g_members = nullptr;
HWND g_input = nullptr;
HWND g_send = nullptr;
HWND g_status = nullptr;
HWND g_sendfile = nullptr;
HWND g_progress = nullptr;
HWND g_play = nullptr;
HFONT g_font = nullptr;
WNDPROC g_inputProc = nullptr;  // 输入框原窗口过程（手动子类化：回车即发送）
std::vector<core::Member> g_rows;  // 与成员列表行一一对应（右键踢人用）
std::wstring g_lastReceived;       // 最近一次接收完成的文件（播放按钮）
bool g_kicked = false;
ULONGLONG g_kickExitAt = 0;

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

void append_system(const std::wstring& msg) {
  append_chat_line(time_prefix() + L"[系统] " + msg);
}

void update_status() {
  App& a = app();
  auto pct = [](uint64_t d, uint64_t t) {
    return std::to_wstring(t ? static_cast<int>(d * 100 / t) : 0) + L"%";
  };
  std::wstring s = L"已加入 " + util::utf8_to_wide(net::kDefaultGroup) + L":" +
                   std::to_wstring(static_cast<int>(net::kDefaultPort)) +
                   L"   昵称: " + util::utf8_to_wide(a.name) +
                   (core::members().is_host() ? L"（房主）" : L"") +
                   L"   成员: " + std::to_wstring(core::members().count()) +
                   L"   收到 " + std::to_wstring(a.rxCount.load()) + L" 条聊天";
  if (core::filexfer().receiving()) {
    s += L"   ⇨ 接收 " + util::utf8_to_wide(core::filexfer().recv_name()) + L" " +
         pct(core::filexfer().recv_done(), core::filexfer().recv_total());
  } else if (core::filexfer().sending()) {
    s += L"   ⇨ 发送 " + util::utf8_to_wide(core::filexfer().send_name()) + L" " +
         pct(core::filexfer().send_done(), core::filexfer().send_total());
  }
  if (core::filexfer().retx_round() > 0)
    s += L"（重传第 " + std::to_wstring(core::filexfer().retx_round()) + L" 轮）";
  SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)s.c_str());
}

// 按成员表重建列表：昵称 / 地址 / 状态 / 角色
void refresh_member_list() {
  g_rows = core::members().snapshot();
  ListView_DeleteAllItems(g_members);
  const proto::Uuid hostId = core::members().host_id();
  for (size_t i = 0; i < g_rows.size(); ++i) {
    const core::Member& m = g_rows[i];
    std::wstring name = util::utf8_to_wide(m.name) + (m.self ? L"（我）" : L"");
    std::wstring ip = m.self ? L"(本机)" : util::utf8_to_wide(net::addr_to_string(m.ip));
    std::wstring status = m.online ? L"在线" : L"离线";
    const wchar_t* role = (m.id == hostId) ? L"房主" : L"成员";

    LVITEMW it{};
    it.mask = LVIF_TEXT;
    it.iItem = static_cast<int>(i);
    it.pszText = (LPWSTR)name.c_str();
    ListView_InsertItem(g_members, &it);
    ListView_SetItemText(g_members, static_cast<int>(i), 1, (LPWSTR)ip.c_str());
    ListView_SetItemText(g_members, static_cast<int>(i), 2, (LPWSTR)status.c_str());
    ListView_SetItemText(g_members, static_cast<int>(i), 3, (LPWSTR)role);
  }
  update_status();
}

void on_kicked_self(const std::wstring& hostName) {
  g_kicked = true;
  g_kickExitAt = GetTickCount64() + 3000;
  append_system(L"你已被房主 " + hostName + L" 移出聊天组，3 秒后自动退出");
  EnableWindow(g_input, FALSE);
  EnableWindow(g_send, FALSE);
  update_status();
  util::dlog("ui: kicked by host, will exit");
}

void handle_member_events() {
  auto events = core::members().drain_events();
  if (events.empty()) return;
  for (const auto& e : events) {
    std::wstring name = util::utf8_to_wide(e.name);
    switch (e.kind) {
      case core::MemberEvent::Joined:      append_system(name + L" 加入了聊天组"); break;
      case core::MemberEvent::Left:        append_system(name + L" 离开了聊天组"); break;
      case core::MemberEvent::Kicked:      append_system(name + L" 被移出聊天组"); break;
      case core::MemberEvent::Offline:     append_system(name + L" 离线"); break;
      case core::MemberEvent::Online:      append_system(name + L" 恢复在线"); break;
      case core::MemberEvent::HostChanged: append_system(name + L" 成为房主"); break;
      case core::MemberEvent::KickedSelf:  on_kicked_self(name); break;
    }
  }
  refresh_member_list();
}

void do_kick_row(int row) {
  if (row < 0 || row >= static_cast<int>(g_rows.size())) return;
  std::string err;
  if (!core::members().kick(g_rows[row].id, &err)) {
    MessageBoxW(app().hwnd, util::utf8_to_wide(err).c_str(), L"无法踢出", MB_ICONWARNING);
    return;
  }
  handle_member_events();  // 立即刷新本地 UI（不等 1s 节拍）
}

void handle_xfer_events() {
  for (const auto& e : core::filexfer().drain_events()) {
    std::wstring name = util::utf8_to_wide(e.name);
    switch (e.kind) {
      case core::XferEvent::SendStarted:
        SendMessageW(g_progress, PBM_SETPOS, 0, 0);
        append_system(L"开始发送 " + name);
        break;
      case core::XferEvent::SendDone:
        append_system(name + L" 发送完成");
        break;
      case core::XferEvent::SendFailed:
        append_system(name + L" 发送失败：" + util::utf8_to_wide(e.detail));
        break;
      case core::XferEvent::RecvStarted: {
        SendMessageW(g_progress, PBM_SETPOS, 0, 0);
        EnableWindow(g_play, FALSE);
        std::wstring from = util::utf8_to_wide(core::members().name_of(e.sender));
        append_system(L"开始接收 " + name + L"（来自 " +
                      (from.empty() ? std::wstring(L"未知成员") : from) + L"）");
        break;
      }
      case core::XferEvent::RecvNack:
        append_system(name + L" " + util::utf8_to_wide(e.detail));
        break;
      case core::XferEvent::RecvDone:
        append_system(name + L" 接收完成，已保存到 " + util::utf8_to_wide(e.path) +
                      L"，可点【▶ 播放】观看");
        g_lastReceived = util::utf8_to_wide(e.path);
        EnableWindow(g_play, TRUE);
        break;
      case core::XferEvent::RecvFailed:
        append_system(name + L" 接收失败：" + util::utf8_to_wide(e.detail));
        break;
    }
  }
}

void on_send_file(HWND hwnd) {
  wchar_t file[MAX_PATH] = L"";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd;
  ofn.lpstrFilter = L"所有文件 (*.*)\0*.*\0\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
  if (!GetOpenFileNameW(&ofn)) return;
  std::string err;
  if (!core::filexfer().start_send(util::wide_to_utf8(file), &err))
    MessageBoxW(hwnd, util::utf8_to_wide(err).c_str(), L"无法发送", MB_ICONWARNING);
}

void on_send() {
  wchar_t buf[4096];
  int n = GetWindowTextW(g_input, buf, 4096);
  std::wstring text = trim(std::wstring(buf, static_cast<size_t>(n)));
  if (text.empty()) return;

  std::string err;
  if (core::send_text(util::wide_to_utf8(text), &err)) {
    append_chat_line(time_prefix() + L"我: " + text);  // 本地回显；回环包按 UUID 丢弃
    SetWindowTextW(g_input, L"");
  } else {
    MessageBoxW(app().hwnd, util::utf8_to_wide("发送失败: " + err).c_str(), L"错误", MB_ICONERROR);
  }
}

// 接收线程投递上来的报文（raw 为堆指针，本函数负责释放）
void handle_rx(RxPacket* raw) {
  App& a = app();
  proto::Packet pkt;
  if (proto::parse_packet(raw->data, raw->len, pkt) && pkt.hdr.sender != a.uuid) {
    switch (pkt.hdr.type) {
      case proto::MSG_CHAT_TEXT: {
        proto::Reader r(pkt.payload, pkt.payload_len);
        std::string name, text;
        if (r.utf8(name) && r.utf8(text) && r.ok()) {
          a.rxCount.fetch_add(1);
          append_chat_line(time_prefix() + util::utf8_to_wide(name) + L": " +
                           util::utf8_to_wide(text));
          update_status();
        }
        break;
      }
      case proto::MSG_HELLO:
      case proto::MSG_HELLO_ACK:
      case proto::MSG_HEARTBEAT:
      case proto::MSG_BYE:
      case proto::MSG_KICK: {
        proto::Reader r(pkt.payload, pkt.payload_len);
        core::members().on_packet(pkt.hdr, r, raw->fromIp);
        handle_member_events();
        if (core::members().take_dirty()) refresh_member_list();  // 静默变化（ACK/心跳入表）
        break;
      }
      case proto::MSG_FILE_META:
      case proto::MSG_FILE_DATA:
      case proto::MSG_FILE_END:
      case proto::MSG_NACK:
      case proto::MSG_FILE_CANCEL: {
        proto::Reader r(pkt.payload, pkt.payload_len);
        core::filexfer().on_packet(pkt.hdr, r, raw->fromIp);
        handle_xfer_events();
        break;
      }
      default:
        break;  // KEY_DIST 由 M4 接管
    }
  }
  free(raw);
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
      WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL,
      0, 0, 0, 0, hwnd, (HMENU)IDC_MEMBERS, hInst, nullptr);
  ListView_SetExtendedListViewStyle(g_members, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
  LVCOLUMNW col{};
  col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
  col.pszText = (LPWSTR)L"昵称"; col.cx = 105; col.iSubItem = 0;
  ListView_InsertColumn(g_members, 0, &col);
  col.pszText = (LPWSTR)L"地址"; col.cx = 110; col.iSubItem = 1;
  ListView_InsertColumn(g_members, 1, &col);
  col.pszText = (LPWSTR)L"状态"; col.cx = 45; col.iSubItem = 2;
  ListView_InsertColumn(g_members, 2, &col);
  col.pszText = (LPWSTR)L"角色"; col.cx = 45; col.iSubItem = 3;
  ListView_InsertColumn(g_members, 3, &col);

  g_input = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, (HMENU)IDC_INPUT, hInst, nullptr);
  g_send = CreateWindowExW(0, L"BUTTON", L"发送",
      WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd, (HMENU)IDC_SEND, hInst, nullptr);

  g_sendfile = CreateWindowExW(0, L"BUTTON", L"发文件…",
      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, (HMENU)IDC_SENDFILE, hInst, nullptr);
  g_progress = CreateWindowExW(0, PROGRESS_CLASSW, L"",
      WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 0, 0, hwnd, (HMENU)IDC_PROGRESS, hInst, nullptr);
  SendMessageW(g_progress, PBM_SETRANGE32, 0, MAKELPARAM(0, 100));
  SendMessageW(g_progress, PBM_SETPOS, 0, 0);
  g_play = CreateWindowExW(0, L"BUTTON", L"▶ 播放",
      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED, 0, 0, 0, 0, hwnd, (HMENU)IDC_PLAY,
      hInst, nullptr);

  g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE,
      0, 0, 0, 0, hwnd, (HMENU)IDC_STATUS, hInst, nullptr);

  g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
  HWND ctrls[] = {g_chat, g_members, g_input, g_send, g_sendfile, g_play, g_status};
  for (HWND h : ctrls) SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
}

void layout(HWND hwnd) {
  if (!g_chat || !g_members || !g_input || !g_send || !g_status || !g_progress) return;
  RECT rc;
  GetClientRect(hwnd, &rc);
  const int pad = 8, listW = 320, btnW = 96, inputH = 30, xferH = 30, xfBtnW = 88;
  SendMessageW(g_status, WM_SIZE, 0, 0);  // 状态栏自适应宽度
  RECT sr;
  GetWindowRect(g_status, &sr);
  int statusH = sr.bottom - sr.top;
  int bottom = rc.bottom - statusH - pad;
  int inputY = bottom - inputH;
  int xferY = inputY - pad - xferH;         // 传输行（发文件/进度/播放）
  int chatH = xferY - pad * 2;              // 聊天区/成员列表高度
  int chatW = rc.right - listW - pad * 3;

  MoveWindow(g_chat, pad, pad, chatW, chatH, TRUE);
  MoveWindow(g_members, pad * 2 + chatW, pad, listW, chatH, TRUE);
  MoveWindow(g_sendfile, pad, xferY, xfBtnW, xferH, TRUE);
  MoveWindow(g_progress, pad + xfBtnW + pad, xferY, chatW - xfBtnW * 2 - pad * 2, xferH, TRUE);
  MoveWindow(g_play, pad + chatW - xfBtnW, xferY, xfBtnW, xferH, TRUE);
  MoveWindow(g_input, pad, inputY, chatW, inputH, TRUE);
  MoveWindow(g_send, pad * 2 + chatW, inputY, btnW, inputH, TRUE);
}

void show_member_menu(HWND hwnd, POINT pt) {
  POINT client = pt;
  ScreenToClient(g_members, &client);
  LVHITTESTINFO ht{};
  ht.pt = client;
  int row = ListView_HitTest(g_members, &ht);
  if (row < 0 || row >= static_cast<int>(g_rows.size())) return;

  const bool canKick = core::members().is_host() && !g_rows[row].self;
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING | (canKick ? MF_ENABLED : MF_GRAYED), IDM_KICK, L"踢出该成员");
  int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
  DestroyMenu(menu);
  if (cmd == IDM_KICK && canKick) do_kick_row(row);
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
      refresh_member_list();
      SetTimer(hwnd, TIMER_TICK, 1000, nullptr);  // 1s 节拍：心跳/超时/被踢退出
      return 0;
    }
    case WM_SIZE:
      if (w != SIZE_MINIMIZED) layout(hwnd);
      return 0;
    case WM_SETFOCUS:
      SetFocus(g_input);
      return 0;
    case WM_TIMER:
      if (w == TIMER_TICK) {
        if (g_kicked && GetTickCount64() >= g_kickExitAt) {
          DestroyWindow(hwnd);
          return 0;
        }
        core::members().tick();
        handle_member_events();
        if (core::members().take_dirty()) refresh_member_list();
        core::filexfer().tick();
        handle_xfer_events();
        // 进度条：接收优先，其次发送
        int pos = 0;
        if (core::filexfer().receiving())
          pos = static_cast<int>(core::filexfer().recv_done() * 100 /
                                 (core::filexfer().recv_total() ? core::filexfer().recv_total()
                                                                : 1));
        else if (core::filexfer().sending())
          pos = static_cast<int>(core::filexfer().send_done() * 100 /
                                 (core::filexfer().send_total() ? core::filexfer().send_total()
                                                                : 1));
        SendMessageW(g_progress, PBM_SETPOS, pos, 0);
        update_status();
      }
      return 0;
    case WM_APP_RX:
      handle_rx(reinterpret_cast<RxPacket*>(l));
      return 0;
    case WM_APP_KICK_ROW:  // 测试钩子：与右键菜单走同一踢人流程
      do_kick_row(static_cast<int>(w));
      return 0;
    case WM_CONTEXTMENU:
      if ((HWND)w == g_members) {
        POINT pt;
        if (static_cast<int>(l) == -1) {
          GetCursorPos(&pt);
        } else {
          pt.x = GET_X_LPARAM(l);
          pt.y = GET_Y_LPARAM(l);
        }
        show_member_menu(hwnd, pt);
        return 0;
      }
      break;
    case WM_COMMAND:
      if (LOWORD(w) == IDC_SEND && HIWORD(w) == BN_CLICKED) on_send();
      else if (LOWORD(w) == IDC_SENDFILE && HIWORD(w) == BN_CLICKED) on_send_file(hwnd);
      else if (LOWORD(w) == IDC_PLAY && HIWORD(w) == BN_CLICKED && !g_lastReceived.empty())
        ShellExecuteW(hwnd, L"open", g_lastReceived.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      return 0;
    case WM_DESTROY: {
      KillTimer(hwnd, TIMER_TICK);
      core::filexfer().shutdown();  // 停发送线程（可能还在发包）
      core::members().shutdown();   // 发送 BYE：其他成员立即移除我
      net::stop_rx_thread();
      app().sock.stop();
      // 清掉消息队列里尚未处理的 WM_APP_RX，避免退出时泄漏
      MSG m;
      while (PeekMessageW(&m, hwnd, WM_APP_RX, WM_APP_RX, PM_REMOVE))
        free(reinterpret_cast<RxPacket*>(m.lParam));
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
                              CW_USEDEFAULT, CW_USEDEFAULT, 1040, 660,
                              nullptr, nullptr, hInst, nullptr);
  if (!hwnd) return false;
  ShowWindow(hwnd, nCmdShow);
  UpdateWindow(hwnd);
  return true;
}

}  // namespace ui
