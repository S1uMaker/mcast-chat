#include "chat_ctrl.hpp"
#include "../app.hpp"
#include "../protocol/codec.hpp"
#include "../util/text.hpp"

namespace core {

bool send_text(const std::string& utf8Text, std::string* err) {
  App& a = app();

  std::vector<uint8_t> payload;
  proto::Writer w(payload);
  w.utf8(a.name);                              // 显示名随包携带（M2 的成员表只做兜底）
  w.utf8(util::utf8_truncate(utf8Text, 1200)); // 保证整包不超过 kMaxDgram
  if (proto::kHeaderSize + payload.size() > proto::kMaxDgram) {
    if (err) *err = "消息过长";
    return false;
  }

  proto::Header h;
  h.type = proto::MSG_CHAT_TEXT;
  h.flags = 0;  // M4 加密后置 FLAG_ENCRYPTED
  h.sender = a.uuid;
  h.seq = a.seq.fetch_add(1);
  std::vector<uint8_t> packet = proto::encode_packet(h, payload);
  return a.sock.send(packet.data(), packet.size(), err);
}

}  // namespace core
