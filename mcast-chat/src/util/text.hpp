#pragma once
#include <cstddef>
#include <string>

namespace util {

// 程序内部统一使用 UTF-8，只在 Win32 宽字符 API 交界处转换
std::wstring utf8_to_wide(const std::string& s);
std::string wide_to_utf8(const std::wstring& s);

// 按码点边界截断，不会撕开多字节字符
std::string utf8_truncate(const std::string& s, size_t maxBytes);

// 进程 exe 所在目录（不含末尾反斜杠）。
// received\ 与 debug.log 等一律锚定在此：文件对话框会改变进程工作目录，
// 用相对路径会让产物散落到任意位置。
std::wstring exe_dir_w();

}  // namespace util
