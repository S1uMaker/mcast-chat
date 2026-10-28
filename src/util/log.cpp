#include "log.hpp"
#include <windows.h>
#include <cstdarg>
#include <cstdio>

namespace util {

void dlog(const char* fmt, ...) {
  FILE* f = fopen("debug.log", "a");
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
