#include "cng_aes.hpp"
#include <windows.h>
#include <bcrypt.h>

namespace crypto {
namespace {

// 进程内共享的 GCM 模式 AES 算法句柄（CNG 保证并发调用安全）
BCRYPT_ALG_HANDLE gcm_alg() {
  static BCRYPT_ALG_HANDLE h = []() -> BCRYPT_ALG_HANDLE {
    BCRYPT_ALG_HANDLE a = nullptr;
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return nullptr;
    if (BCryptSetProperty(a, (LPCWSTR)BCRYPT_CHAINING_MODE,
                          (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                          (ULONG)sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0) {
      BCryptCloseAlgorithmProvider(a, 0);
      return nullptr;
    }
    return a;
  }();
  return h;
}

}  // namespace

bool random_bytes(uint8_t* out, size_t n) {
  if (n == 0) return true;
  return BCryptGenRandom(nullptr, (PUCHAR)out, static_cast<ULONG>(n),
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

bool aes256gcm_encrypt(const std::string& key32, const uint8_t nonce[12],
                       const uint8_t* aad, size_t aadLen,
                       const void* plain, size_t plainLen, std::string& out) {
  BCRYPT_ALG_HANDLE alg = gcm_alg();
  if (!alg || key32.size() != 32 || plainLen == 0) return false;
  BCRYPT_KEY_HANDLE hk = nullptr;
  if (BCryptGenerateSymmetricKey(alg, &hk, nullptr, 0, (PUCHAR)key32.data(), 32, 0) != 0)
    return false;
  bool ok = false;
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
  BCRYPT_INIT_AUTH_MODE_INFO(info);
  info.pbNonce = (PUCHAR)nonce;
  info.cbNonce = 12;
  info.pbAuthData = (PUCHAR)aad;
  info.cbAuthData = static_cast<ULONG>(aadLen);
  std::string tag(16, '\0');
  info.pbTag = (PUCHAR)tag.data();
  info.cbTag = 16;
  out.assign(plainLen, '\0');
  ULONG done = 0;
  if (BCryptEncrypt(hk, (PUCHAR)plain, static_cast<ULONG>(plainLen), &info, nullptr, 0,
                    (PUCHAR)&out[0], static_cast<ULONG>(plainLen), &done, 0) == 0) {
    out += tag;
    ok = true;
  }
  BCryptDestroyKey(hk);
  return ok;
}

bool aes256gcm_decrypt(const std::string& key32, const uint8_t nonce[12],
                       const uint8_t* aad, size_t aadLen,
                       const void* cipherTag, size_t len, std::string& plain) {
  BCRYPT_ALG_HANDLE alg = gcm_alg();
  if (!alg || key32.size() != 32 || len <= 16) return false;  // 至少要有 tag
  BCRYPT_KEY_HANDLE hk = nullptr;
  if (BCryptGenerateSymmetricKey(alg, &hk, nullptr, 0, (PUCHAR)key32.data(), 32, 0) != 0)
    return false;
  bool ok = false;
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
  BCRYPT_INIT_AUTH_MODE_INFO(info);
  info.pbNonce = (PUCHAR)nonce;
  info.cbNonce = 12;
  info.pbAuthData = (PUCHAR)aad;
  info.cbAuthData = static_cast<ULONG>(aadLen);
  info.pbTag = (PUCHAR)cipherTag + (len - 16);
  info.cbTag = 16;
  ULONG ctLen = static_cast<ULONG>(len - 16);
  plain.assign(ctLen, '\0');
  ULONG done = 0;
  if (BCryptDecrypt(hk, (PUCHAR)cipherTag, ctLen, &info, nullptr, 0,
                    (PUCHAR)&plain[0], ctLen, &done, 0) == 0) {
    ok = true;
  } else {
    plain.clear();  // tag 不匹配：密钥不对或数据被篡改
  }
  BCryptDestroyKey(hk);
  return ok;
}

}  // namespace crypto
