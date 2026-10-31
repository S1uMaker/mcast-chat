#include "secure_session.hpp"
#include <algorithm>
#include <cstring>
#include <vector>
#include "../app.hpp"
#include <rpc.h>
#include "../protocol/codec.hpp"
#include "../util/log.hpp"
#include "cng_aes.hpp"
#include "sha256.hpp"

namespace crypto {

namespace {

uint64_t now_tick() { return GetTickCount64(); }

std::string id_hex(const proto::Uuid& id) { return proto::uuid_to_string(id); }

proto::Uuid uuid_from_hex(const std::string& hex) {
  proto::Uuid id{};
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
  };
  for (int i = 0; i < 16 && 2 * i + 1 < static_cast<int>(hex.size()); ++i)
    id[i] = static_cast<uint8_t>((nib(hex[2 * i]) << 4) | nib(hex[2 * i + 1]));
  return id;
}

bool read_blob(proto::Reader& r, std::string& out) {
  uint16_t n = 0;
  if (!r.u16(n)) return false;
  out.assign(n, '\0');
  if (n > 0 && !r.bytes(reinterpret_cast<uint8_t*>(&out[0]), n)) return false;
  return true;
}

bool is_data_plane(uint8_t type) {
  return type == proto::MSG_CHAT_TEXT ||
         (type >= proto::MSG_FILE_META && type <= proto::MSG_FILE_CANCEL);
}

void nonce_from(const proto::Uuid& sender, uint32_t seq, uint8_t out[12]) {
  memcpy(out, sender.data(), 8);
  out[8] = static_cast<uint8_t>(seq >> 24);
  out[9] = static_cast<uint8_t>(seq >> 16);
  out[10] = static_cast<uint8_t>(seq >> 8);
  out[11] = static_cast<uint8_t>(seq);
}

void aad_from(const proto::Uuid& sender, uint32_t seq, uint8_t type, std::string& aad) {
  aad.assign(reinterpret_cast<const char*>(sender.data()), 16);
  aad += static_cast<char>(seq >> 24);
  aad += static_cast<char>(seq >> 16);
  aad += static_cast<char>(seq >> 8);
  aad += static_cast<char>(seq);
  aad += static_cast<char>(type);
}

}  // namespace

SecureSession& session() {
  static SecureSession s;
  return s;
}

void SecureSession::init() {
  if (rsa_) return;
  if (!rsa_.generate()) {
    util::dlog("session: RSA keygen FAILED (encryption disabled)");
    return;
  }
  rsa_.export_public(pubBlob_);
  util::dlog("session: RSA-2048 ready, pub blob %zu B", pubBlob_.size());
}

bool SecureSession::have_key() const {
  std::lock_guard<std::mutex> g(keyMtx_);
  return haveKey_;
}

KeySnapshot SecureSession::current_key() const {
  std::lock_guard<std::mutex> g(keyMtx_);
  if (!haveKey_) return {};
  return {keyId_, groupKey_};
}

void SecureSession::set_member_key(const proto::Uuid& id, const std::string& pubBlob) {
  if (id == app().uuid || pubBlob.empty()) return;
  memberPubs_[id_hex(id)] = pubBlob;
}

std::string SecureSession::id_hex_(const proto::Uuid& id) { return proto::uuid_to_string(id); }

void SecureSession::drop_member_key(const proto::Uuid& id) { memberPubs_.erase(id_hex(id)); }

void SecureSession::on_membership_changed() { rekeyPending_ = true; }

void SecureSession::tick(bool amHost) {
  if (amHost && enabled_ && rekeyPending_ &&
      now_tick() - lastRekeyTick_ >= 2000) {  // 去抖：多实例接连启动时不刷屏
    rekeyPending_ = false;
    distribute_now();
  }
}

void SecureSession::gen_group_key_locked() {
  std::lock_guard<std::mutex> g(keyMtx_);
  prevKeyId_ = keyId_;
  prevKey_ = groupKey_;
  keyId_ = keyId_ + 1;
  groupKey_.resize(32);
  if (!random_bytes(reinterpret_cast<uint8_t*>(&groupKey_[0]), 32)) {
    // 兜底：两个 UUID 拼接（密码学强度弱于 RNG，仅极端情况）
    UUID u1, u2;
    UuidCreate(&u1);
    UuidCreate(&u2);
    memcpy(&groupKey_[0], &u1, 16);
    memcpy(&groupKey_[0] + 16, &u2, 16);
  }
  haveKey_ = true;
}

// 房主：生成新组密钥 → 对每个在线成员的公钥做 RSA-OAEP 信封 → KEY_DIST 多播
void SecureSession::distribute_now() {
  if (!rsa_) return;
  gen_group_key_locked();
  lastRekeyTick_ = now_tick();

  std::string key;
  uint32_t kid = 0;
  {
    std::lock_guard<std::mutex> g(keyMtx_);
    key = groupKey_;
    kid = keyId_;
  }

  std::vector<std::pair<proto::Uuid, std::string>> envelopes;
  for (auto& kv : memberPubs_) {
    proto::Uuid id = uuid_from_hex(kv.first);
    if (id == app().uuid) continue;  // 自己已有密钥
    RsaKey pub;
    if (!pub.import_public(kv.second)) continue;
    std::string enc;
    if (!pub.encrypt(key, enc)) continue;
    envelopes.push_back({id, enc});
  }

  const size_t perEntry = 16 + 2 + (envelopes.empty() ? 256 : envelopes[0].second.size());
  const size_t perPacket = std::max<size_t>(1, (1300 - 8) / perEntry);
  for (size_t i = 0; i < envelopes.size();) {
    std::vector<uint8_t> p;
    proto::Writer w(p);
    w.u32(kid);
    uint32_t n = static_cast<uint32_t>(std::min<size_t>(envelopes.size() - i, perPacket));
    w.u32(n);
    for (uint32_t k = 0; k < n; ++k, ++i) {
      w.bytes(envelopes[i].first.data(), 16);
      w.u16(static_cast<uint16_t>(envelopes[i].second.size()));
      w.bytes(envelopes[i].second.data(), envelopes[i].second.size());
    }
    send_raw(proto::MSG_KEY_DIST, std::string(p.begin(), p.end()));
  }
  util::dlog("session: distributed key #%u to %zu member(s)", kid, envelopes.size());
}

void SecureSession::handle_key_dist(proto::Reader& r) {
  uint32_t keyId = 0, count = 0;
  if (!(r.u32(keyId) && r.u32(count))) return;
  for (uint32_t i = 0; i < count && i < 1000; ++i) {
    uint8_t tid[16];
    std::string blob;
    if (!(r.bytes(tid, 16) && read_blob(r, blob))) return;
    if (memcmp(tid, app().uuid.data(), 16) != 0) continue;

    std::string key;
    if (!rsa_.decrypt(blob, key) || key.size() != 32) {
      util::dlog("session: KEY_DIST envelope decrypt FAILED (key #%u)", keyId);
      return;
    }
    std::lock_guard<std::mutex> g(keyMtx_);
    if (keyId > keyId_) {  // 只接受更新的密钥（旧包/重放忽略）
      prevKeyId_ = keyId_;
      prevKey_ = groupKey_;
      groupKey_ = key;
      keyId_ = keyId;
      haveKey_ = true;
      util::dlog("session: got group key #%u", keyId);
    }
    return;  // 自己的信封只会有一个
  }
}

// ---------------- 签名 ----------------

bool SecureSession::sign_data(const std::string& data, std::string& sig) const {
  if (!rsa_) return false;
  std::string hash = sha256_bytes(data.data(), data.size());
  if (hash.size() != 32) return false;
  return rsa_.sign_hash(reinterpret_cast<const uint8_t*>(hash.data()), sig);
}

bool SecureSession::verify_data(const proto::Uuid& signer, const std::string& data,
                                const std::string& sig) const {
  auto it = memberPubs_.find(id_hex(signer));
  if (it == memberPubs_.end()) return false;  // 未知公钥：无法验证
  RsaKey pub;
  if (!pub.import_public(it->second)) return false;
  std::string hash = sha256_bytes(data.data(), data.size());
  if (hash.size() != 32) return false;
  return pub.verify_hash(reinterpret_cast<const uint8_t*>(hash.data()), sig);
}

// ---------------- 数据面 seal / unseal ----------------

std::string SecureSession::encrypt_with(const KeySnapshot& ks, const proto::Uuid& sender,
                                        uint32_t seq, uint8_t type,
                                        const std::string& plain) const {
  if (ks.key.size() != 32 || plain.empty()) return "";
  uint8_t nonce[12];
  nonce_from(sender, seq, nonce);
  std::string aad;
  aad_from(sender, seq, type, aad);
  std::string out;
  if (!aes256gcm_encrypt(ks.key, nonce, reinterpret_cast<const uint8_t*>(aad.data()),
                         aad.size(), plain.data(), plain.size(), out))
    return "";
  return out;
}

std::string SecureSession::decrypt_with(uint32_t keyId, const proto::Uuid& sender, uint32_t seq,
                                        uint8_t type, const std::string& cipher) const {
  std::string key;
  {
    std::lock_guard<std::mutex> g(keyMtx_);
    if (keyId == keyId_ && haveKey_)
      key = groupKey_;
    else if (keyId == prevKeyId_)
      key = prevKey_;  // 重分发瞬间的在途包用旧密钥解
    else
      return "";
  }
  if (cipher.size() <= 16) return "";
  uint8_t nonce[12];
  nonce_from(sender, seq, nonce);
  std::string aad;
  aad_from(sender, seq, type, aad);
  std::string plain;
  if (!aes256gcm_decrypt(key, nonce, reinterpret_cast<const uint8_t*>(aad.data()), aad.size(),
                         cipher.data(), cipher.size(), plain))
    return "";
  return plain;
}

std::string SecureSession::seal_payload(uint8_t type, const proto::Uuid& sender, uint32_t seq,
                                        const std::string& plain, uint16_t& flags,
                                        const KeySnapshot* keyOverride) {
  flags = 0;
  std::string body = plain;
  if (is_data_plane(type)) {
    KeySnapshot ks = keyOverride ? *keyOverride : current_key();
    if (enabled_ && !ks.key.empty()) {
      std::string ct = encrypt_with(ks, sender, seq, type, plain);
      if (!ct.empty()) {
        char kib[4] = {static_cast<char>(ks.keyId >> 24), static_cast<char>(ks.keyId >> 16),
                       static_cast<char>(ks.keyId >> 8), static_cast<char>(ks.keyId)};
        body = std::string(kib, 4) + ct;
        flags |= proto::FLAG_ENCRYPTED;
      }
    }
  }
  if (type == proto::MSG_CHAT_TEXT) {  // TEXT 恒签名（防伪造发送者）
    std::string sig;
    if (sign_data(plain, sig)) {
      body += sig;
      flags |= proto::FLAG_SIGNED;
    }
  }
  return body;
}

bool SecureSession::unseal(const proto::Header& h, const void* payload, size_t len,
                           std::string& plain) const {
  std::string body(reinterpret_cast<const char*>(payload), len);
  std::string sig;
  if (h.flags & proto::FLAG_SIGNED) {
    if (body.size() < 256) return false;
    sig = body.substr(body.size() - 256);
    body.resize(body.size() - 256);
  }
  if (h.flags & proto::FLAG_ENCRYPTED) {
    if (body.size() < 4) return false;
    uint32_t keyId = (static_cast<uint8_t>(body[0]) << 24) |
                     (static_cast<uint8_t>(body[1]) << 16) |
                     (static_cast<uint8_t>(body[2]) << 8) | static_cast<uint8_t>(body[3]);
    std::string ct = body.substr(4);
    plain = decrypt_with(keyId, h.sender, h.seq, h.type, ct);
    if (plain.empty()) return false;  // 密钥不对/被篡改/未持有
  } else {
    plain = body;
  }
  if ((h.flags & proto::FLAG_SIGNED) && !verify_data(h.sender, plain, sig)) {
    util::dlog("session: signature INVALID from %s", proto::uuid_to_string(h.sender).c_str());
    return false;
  }
  return true;
}

void SecureSession::send_raw(uint8_t type, const std::string& payload) const {
  proto::Header h;
  h.type = type;
  h.flags = 0;
  h.sender = app().uuid;
  h.seq = app().seq.fetch_add(1);
  std::vector<uint8_t> p(payload.begin(), payload.end());
  auto pkt = proto::encode_packet(h, p);
  app().sock.send(pkt.data(), pkt.size());
}

}  // namespace crypto
