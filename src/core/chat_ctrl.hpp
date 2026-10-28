#pragma once
#include <string>

namespace core {

// 组包并多播一条聊天文本；失败时 err 带原因
bool send_text(const std::string& utf8Text, std::string* err = nullptr);

}  // namespace core
