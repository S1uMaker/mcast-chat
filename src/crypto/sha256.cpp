#include "sha256.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <cstdio>
#include <vector>
#include "../util/text.hpp"

namespace crypto {

std::string sha256_file(const std::string& utf8Path, std::string* err) {
  std::string digest;
  FILE* f = _wfopen(util::utf8_to_wide(utf8Path).c_str(), L"rb");
  if (!f) {
    if (err) *err = "无法打开文件";
    return digest;
  }

  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::vector<uint8_t> hashObj;
  std::vector<uint8_t> buf(1 << 20);  // 1MB 流式读取
  bool ok = false;

  do {
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
      if (err) *err = "CNG 打开 SHA-256 算法失败";
      break;
    }
    DWORD objLen = 0, cb = 0;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen), &cb, 0) != 0) {
      if (err) *err = "CNG 获取对象长度失败";
      break;
    }
    hashObj.resize(objLen);
    if (BCryptCreateHash(alg, &hash, hashObj.data(), objLen, nullptr, 0, 0) != 0) {
      if (err) *err = "CNG 创建哈希失败";
      break;
    }
    size_t n;
    bool hashErr = false;
    while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) {
      if (BCryptHashData(hash, buf.data(), static_cast<ULONG>(n), 0) != 0) {
        if (err) *err = "CNG 计算哈希失败";
        hashErr = true;
        break;
      }
    }
    if (hashErr) break;
    digest.resize(32);
    if (BCryptFinishHash(hash, (PUCHAR)&digest[0], 32, 0) != 0) {
      if (err) *err = "CNG 完成哈希失败";
      digest.clear();
      break;
    }
    ok = true;
  } while (false);

  if (hash) BCryptDestroyHash(hash);
  if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  fclose(f);
  return ok ? digest : std::string();
}

}  // namespace crypto
