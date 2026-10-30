#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace crypto {

// 密码学安全随机数（BCryptGenRandom，系统首选 RNG）
bool random_bytes(uint8_t* out, size_t n);

// AES-256-GCM。out = 密文 || tag(16B)；AAD 绑定包头（senderId/seq/type）防裁剪拼接。
bool aes256gcm_encrypt(const std::string& key32, const uint8_t nonce[12],
                       const uint8_t* aad, size_t aadLen,
                       const void* plain, size_t plainLen, std::string& out);
// tag 校验失败（密钥不对/数据被改）返回 false
bool aes256gcm_decrypt(const std::string& key32, const uint8_t nonce[12],
                       const uint8_t* aad, size_t aadLen,
                       const void* cipherTag, size_t len, std::string& plain);

}  // namespace crypto
