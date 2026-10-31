#include "log.hpp"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include "text.hpp"

namespace util {

void dlog(const char* fmt, ...) {
  // 锚定到 exe 目录：进程工作目录会被文件对话框改变
  std::wstring logPath = exe_dir_w() + L"\\debug.log";
  FILE* f = _wfopen(logPath.c_str(), L"a");
  if (!f) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  fprintf(f, "[%02d:%02d:%02d.%03d pid%lu] ", static_cast<int>(t.wHour),
          static_cast<int>(t.wMinute), static_cast<int>(t.wSecond),
          static_cast<int>(t.wMilliseconds),
          static_cast<unsigned long>(GetCurrentProcessId()));
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fputc('\n', f);
  fclose(f);
}

}  // namespace util
