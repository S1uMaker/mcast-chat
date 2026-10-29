#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace crypto {

// 用 Windows CNG（bcrypt）计算文件 SHA-256，返回 32 字节原始摘要。
// 失败返回空串，err 带原因。
std::string sha256_file(const std::string& utf8Path, std::string* err);

}  // namespace crypto
