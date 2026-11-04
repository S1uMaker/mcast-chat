#pragma once
#include <windows.h>

namespace ui {

// 注册窗口类并创建主窗口；成功后 app().hwnd 可用
bool create_main_window(HINSTANCE hInst, int nCmdShow);

}  // namespace ui
