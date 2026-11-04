#pragma once
#include <cstdint>
#include <string>

namespace crypto {

// Windows CNG RSA-2048 封装（RAII）。
// 公钥用 BCRYPT_RSAPUBLIC_BLOB 导出/导入（随 HELLO 广播）；
// 加密 OAEP-SHA256（组密钥信封），签名 PSS-SHA256（TEXT/KICK 防伪造）。
class RsaKey {
 public:
  RsaKey() = default;
  ~RsaKey();
  RsaKey(const RsaKey&) = delete;
  RsaKey& operator=(const RsaKey&) = delete;

  bool generate();                                   // 生成 RSA-2048 密钥对
  bool import_public(const std::string& blob);       // 从公钥 blob 导入
  bool export_public(std::string& out) const;        // 导出公钥 blob

  bool encrypt(const std::string& plain, std::string& out) const;  // OAEP-SHA256（≤190B 明文）
  bool decrypt(const std::string& cipher, std::string& out) const;
  bool sign_hash(const uint8_t hash[32], std::string& sig) const;  // PSS-SHA256（需私钥）
  bool verify_hash(const uint8_t hash[32], const std::string& sig) const;

  explicit operator bool() const { return key_ != nullptr; }

 private:
  void reset();
  void* alg_ = nullptr;  // BCRYPT_ALG_HANDLE
  void* key_ = nullptr;  // BCRYPT_KEY_HANDLE
};

}  // namespace crypto
