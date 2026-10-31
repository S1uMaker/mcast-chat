#include "chat_ctrl.hpp"
#include <vector>
#include "../app.hpp"
#include "../crypto/secure_session.hpp"
#include "../protocol/codec.hpp"
#include "../util/text.hpp"

namespace core {

bool send_text(const std::string& utf8Text, std::string* err) {
  App& a = app();

  // 预算：头 32 + (2+昵称) + (2+正文) + keyId 4 + tag 16 + 签名 ~283 ≤ 1400
  std::vector<uint8_t> plain;
  proto::Writer w(plain);
  w.utf8(a.name);
  w.utf8(util::utf8_truncate(utf8Text, 1000));

  proto::Header h;
  h.type = proto::MSG_CHAT_TEXT;
  h.sender = a.uuid;
  h.seq = a.seq.fetch_add(1);
  uint16_t flags = 0;  // seal_payload 决定加密/签名标志
  std::string body = crypto::session().seal_payload(proto::MSG_CHAT_TEXT, a.uuid, h.seq,
                                                    std::string(plain.begin(), plain.end()), flags);
  h.flags = flags;

  std::vector<uint8_t> payload(body.begin(), body.end());
  if (proto::kHeaderSize + payload.size() > proto::kMaxDgram) {
    if (err) *err = "消息过长";
    return false;
  }
  auto packet = proto::encode_packet(h, payload);
  return a.sock.send(packet.data(), packet.size(), err);
}

}  // namespace core
