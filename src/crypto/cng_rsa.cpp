#include "cng_rsa.hpp"
#include <windows.h>
#include <bcrypt.h>

namespace crypto {

RsaKey::~RsaKey() { reset(); }

void RsaKey::reset() {
  if (key_) { BCryptDestroyKey((BCRYPT_KEY_HANDLE)key_); key_ = nullptr; }
  if (alg_) { BCryptCloseAlgorithmProvider((BCRYPT_ALG_HANDLE)alg_, 0); alg_ = nullptr; }
}

bool RsaKey::generate() {
  reset();
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_KEY_HANDLE key = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, nullptr, 0) != 0) return false;
  if (BCryptGenerateKeyPair(alg, &key, 2048, 0) != 0) {
    BCryptCloseAlgorithmProvider(alg, 0);
    return false;
  }
  if (BCryptFinalizeKeyPair(key, 0) != 0) {
    BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(alg, 0);
    return false;
  }
  alg_ = alg;
  key_ = key;
  return true;
}

bool RsaKey::import_public(const std::string& blob) {
  reset();
  if (blob.empty()) return false;
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, nullptr, 0) != 0) return false;
  BCRYPT_KEY_HANDLE key = nullptr;
  if (BCryptImportKeyPair(alg, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key,
                          (PUCHAR)blob.data(), static_cast<ULONG>(blob.size()), 0) != 0) {
    BCryptCloseAlgorithmProvider(alg, 0);
    return false;
  }
  alg_ = alg;
  key_ = key;
  return true;
}

bool RsaKey::export_public(std::string& out) const {
  if (!key_) return false;
  ULONG cb = 0;
  if (BCryptExportKey((BCRYPT_KEY_HANDLE)key_, nullptr, BCRYPT_RSAPUBLIC_BLOB,
                      nullptr, 0, &cb, 0) != 0 || cb == 0)
    return false;
  out.assign(cb, '\0');
  return BCryptExportKey((BCRYPT_KEY_HANDLE)key_, nullptr, BCRYPT_RSAPUBLIC_BLOB,
                         (PUCHAR)&out[0], cb, &cb, 0) == 0;
}

bool RsaKey::encrypt(const std::string& plain, std::string& out) const {
  if (!key_ || plain.empty() || plain.size() > 190) return false;  // OAEP-SHA256 上限
  BCRYPT_OAEP_PADDING_INFO pi{};
  pi.pszAlgId = L"SHA256";
  ULONG cb = 0;
  if (BCryptEncrypt((BCRYPT_KEY_HANDLE)key_, (PUCHAR)plain.data(),
                    static_cast<ULONG>(plain.size()), &pi, nullptr, 0,
                    nullptr, 0, &cb, BCRYPT_PAD_OAEP) != 0)
    return false;
  out.assign(cb, '\0');
  return BCryptEncrypt((BCRYPT_KEY_HANDLE)key_, (PUCHAR)plain.data(),
                       static_cast<ULONG>(plain.size()), &pi, nullptr, 0,
                       (PUCHAR)&out[0], cb, &cb, BCRYPT_PAD_OAEP) == 0;
}

bool RsaKey::decrypt(const std::string& cipher, std::string& out) const {
  if (!key_ || cipher.empty()) return false;
  BCRYPT_OAEP_PADDING_INFO pi{};
  pi.pszAlgId = L"SHA256";
  ULONG cb = 0;
  if (BCryptDecrypt((BCRYPT_KEY_HANDLE)key_, (PUCHAR)cipher.data(),
                    static_cast<ULONG>(cipher.size()), &pi, nullptr, 0,
                    nullptr, 0, &cb, BCRYPT_PAD_OAEP) != 0)
    return false;
  out.assign(cb, '\0');
  return BCryptDecrypt((BCRYPT_KEY_HANDLE)key_, (PUCHAR)cipher.data(),
                       static_cast<ULONG>(cipher.size()), &pi, nullptr, 0,
                       (PUCHAR)&out[0], cb, &cb, BCRYPT_PAD_OAEP) == 0;
}

bool RsaKey::sign_hash(const uint8_t hash[32], std::string& sig) const {
  if (!key_) return false;
  BCRYPT_PSS_PADDING_INFO pi{};
  pi.pszAlgId = L"SHA256";
  pi.cbSalt = 32;  // 盐长 = 哈希长，PSS 标准
  ULONG cb = 0;
  if (BCryptSignHash((BCRYPT_KEY_HANDLE)key_, &pi, (PUCHAR)hash, 32,
                     nullptr, 0, &cb, BCRYPT_PAD_PSS) != 0)
    return false;
  sig.assign(cb, '\0');
  return BCryptSignHash((BCRYPT_KEY_HANDLE)key_, &pi, (PUCHAR)hash, 32,
                        (PUCHAR)&sig[0], cb, &cb, BCRYPT_PAD_PSS) == 0;
}

bool RsaKey::verify_hash(const uint8_t hash[32], const std::string& sig) const {
  if (!key_ || sig.empty()) return false;
  BCRYPT_PSS_PADDING_INFO pi{};
  pi.pszAlgId = L"SHA256";
  pi.cbSalt = 32;
  return BCryptVerifySignature((BCRYPT_KEY_HANDLE)key_, &pi, (PUCHAR)hash, 32,
                               (PUCHAR)sig.data(), static_cast<ULONG>(sig.size()),
                               BCRYPT_PAD_PSS) == 0;
}

}  // namespace crypto
