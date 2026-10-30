#pragma once
#include <map>
#include <mutex>
#include <string>
#include "../protocol/codec.hpp"
#include "../protocol/message.hpp"
#include "cng_rsa.hpp"

namespace crypto {

// 组密钥快照（发送线程在任务开始时持有，避免与重分发产生数据竞争；
// 重分发后旧密钥仍被接收方作为 previous key 保留，传输中的旧包可继续解密）
struct KeySnapshot {
  uint32_t keyId = 0;
  std::string key;  // 32B；空 = 未持有
};

// 安全会话（M4，需求 d 加分项）：
//   ① 每个成员 RSA-2048 密钥对，公钥随 HELLO 广播（set_member_key 学表）。
//   ② 房主生成 32B 组会话密钥，对每个成员用其公钥 RSA-OAEP 加密（信封），
//      以 KEY_DIST 多播分发；成员用自己私钥解出。
//   ③ 数据面（TEXT/FILE_*）载荷 AES-256-GCM 加密：nonce = senderId[0..7]+seq，
//      AAD = senderId||seq||type（绑定包头防裁剪/拼接/重放）；载荷前缀 keyId，
//      重分发期间旧包用 previous key 继续可解。
//   ④ TEXT 附 RSA-PSS 签名（FLAG_SIGNED）；KICK 由房主签名，全员验签。
//   ⑤ 成员变化 → 房主去抖 2s 后重生成并重分发密钥：被踢者被排除出信封名单，
//      拿不到新密钥 = 应用层强制离组。
class SecureSession {
 public:
  void init();  // 启动时生成 RSA 密钥对（一次）

  const std::string& public_key() const { return pubBlob_; }

  void set_enabled(bool on) { enabled_ = on; }
  bool enabled() const { return enabled_; }
  bool have_key() const;

  void distribute_now();          // 房主：生成新组密钥并信封分发
  void on_membership_changed();   // 成员变化 → 标记待重分发（去抖 2s）
  void tick(bool amHost);         // UI 1s 节拍：房主执行挂起的重分发

  void set_member_key(const proto::Uuid& id, const std::string& pubBlob);
  void drop_member_key(const proto::Uuid& id);
  bool has_member_key(const proto::Uuid& id) const { return memberPubs_.count(id_hex_(id)) > 0; }

  bool sign_data(const std::string& data, std::string& sig) const;
  bool verify_data(const proto::Uuid& signer, const std::string& data,
                   const std::string& sig) const;

  // 数据面打包：TEXT 恒签名；加密开启且持有密钥时 AES-GCM 加密（载荷前缀 keyId）
  std::string seal_payload(uint8_t type, const proto::Uuid& sender, uint32_t seq,
                           const std::string& plain, uint16_t& flags,
                           const KeySnapshot* keyOverride = nullptr);
  // 数据面拆包：解不开/验签不过返回 false
  bool unseal(const proto::Header& h, const void* payload, size_t len,
              std::string& plain) const;

  void handle_key_dist(proto::Reader& r);  // 收到 KEY_DIST：找自己的信封解出组密钥

  KeySnapshot current_key() const;

 private:
  void gen_group_key_locked();
  void send_raw(uint8_t type, const std::string& payload) const;
  std::string encrypt_with(const KeySnapshot& ks, const proto::Uuid& sender, uint32_t seq,
                           uint8_t type, const std::string& plain) const;
  std::string decrypt_with(uint32_t keyId, const proto::Uuid& sender, uint32_t seq,
                           uint8_t type, const std::string& cipher) const;
  static std::string id_hex_(const proto::Uuid& id);

  std::string pubBlob_;
  RsaKey rsa_;
  std::map<std::string, std::string> memberPubs_;  // idhex → 公钥 blob（UI 线程）

  mutable std::mutex keyMtx_;
  std::string groupKey_, prevKey_;
  uint32_t keyId_ = 0, prevKeyId_ = 0;
  bool haveKey_ = false;

  bool enabled_ = false;
  bool rekeyPending_ = false;
  uint64_t lastRekeyTick_ = 0;
};

SecureSession& session();  // 全局单例

}  // namespace crypto
