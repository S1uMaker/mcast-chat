#pragma once

namespace util {

// 调试日志：追加写入 exe 目录下 debug.log（仅排查问题用）
void dlog(const char* fmt, ...);

}  // namespace util
