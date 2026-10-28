#include "text.hpp"
#include <windows.h>

namespace util {

std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
  return w;
}

std::string wide_to_utf8(const std::wstring& s) {
  if (s.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                              nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                 &out[0], n, nullptr, nullptr);
  return out;
}

std::string utf8_truncate(const std::string& s, size_t maxBytes) {
  if (s.size() <= maxBytes) return s;
  size_t cut = maxBytes;
  // 回退到码点起始字节（ continuation 字节形如 10xxxxxx ）
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
  return s.substr(0, cut);
}

}  // namespace util
