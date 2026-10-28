#pragma once
#include <cstddef>
#include <string>

namespace util {

// 程序内部统一使用 UTF-8，只在 Win32 宽字符 API 交界处转换
std::wstring utf8_to_wide(const std::string& s);
std::string wide_to_utf8(const std::wstring& s);

// 按码点边界截断，不会撕开多字节字符
std::string utf8_truncate(const std::string& s, size_t maxBytes);

}  // namespace util
