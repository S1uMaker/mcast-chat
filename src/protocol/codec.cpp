#include "codec.hpp"
#include "message.hpp"

namespace proto {

void Writer::u16(uint16_t v) {
  buf_.push_back(uint8_t(v >> 8));
  buf_.push_back(uint8_t(v));
}

void Writer::u32(uint32_t v) {
  for (int i = 3; i >= 0; --i) buf_.push_back(uint8_t(v >> (8 * i)));
}

void Writer::u64(uint64_t v) {
  for (int i = 7; i >= 0; --i) buf_.push_back(uint8_t(v >> (8 * i)));
}

void Writer::bytes(const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  buf_.insert(buf_.end(), b, b + n);
}

void Writer::utf8(const std::string& s) {
  u16(static_cast<uint16_t>(s.size()));
  bytes(s.data(), s.size());
}

bool Reader::u8(uint8_t& v) {
  if (left_ < 1) { good_ = false; return false; }
  v = *p_++;
  --left_;
  return true;
}

bool Reader::bytes(uint8_t* out, size_t n) {
  if (left_ < n) { good_ = false; return false; }
  for (size_t i = 0; i < n; ++i) out[i] = p_[i];
  p_ += n;
  left_ -= n;
  return true;
}

bool Reader::u16(uint16_t& v) {
  uint8_t b[2];
  if (!bytes(b, 2)) return false;
  v = uint16_t((uint16_t(b[0]) << 8) | b[1]);
  return true;
}

bool Reader::u32(uint32_t& v) {
  uint8_t b[4];
  if (!bytes(b, 4)) return false;
  v = 0;
  for (int i = 0; i < 4; ++i) v = (v << 8) | b[i];
  return true;
}

bool Reader::u64(uint64_t& v) {
  uint8_t b[8];
  if (!bytes(b, 8)) return false;
  v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | b[i];
  return true;
}

bool Reader::utf8(std::string& s) {
  uint16_t n = 0;
  if (!u16(n)) return false;
  if (left_ < n) { good_ = false; return false; }
  s.assign(reinterpret_cast<const char*>(p_), n);
  p_ += n;
  left_ -= n;
  return true;
}

std::vector<uint8_t> encode_packet(const Header& h, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> buf;
  buf.reserve(kHeaderSize + payload.size());
  Writer w(buf);
  w.u32(kMagic);
  w.u8(kVersion);
  w.u8(h.type);
  w.u16(h.flags);
  w.bytes(h.sender.data(), h.sender.size());
  w.u32(h.seq);
  w.u32(static_cast<uint32_t>(payload.size()));
  buf.insert(buf.end(), payload.begin(), payload.end());
  return buf;
}

bool parse_packet(const uint8_t* data, size_t len, Packet& out) {
  if (len < kHeaderSize) return false;
  Reader r(data, len);
  uint32_t magic = 0;
  uint8_t version = 0, type = 0;
  uint16_t flags = 0;
  uint32_t seq = 0, plen = 0;
  if (!r.u32(magic) || magic != kMagic) return false;
  if (!r.u8(version) || version != kVersion) return false;
  if (!r.u8(type) || !r.u16(flags)) return false;
  if (!r.bytes(out.hdr.sender.data(), out.hdr.sender.size())) return false;
  if (!r.u32(seq) || !r.u32(plen)) return false;
  if (r.remaining() != plen) return false;
  out.hdr.type = type;
  out.hdr.flags = flags;
  out.hdr.seq = seq;
  out.payload = data + kHeaderSize;
  out.payload_len = plen;
  return true;
}

std::string uuid_to_string(const Uuid& u) {
  static const char* hex = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < u.size(); ++i) {
    s += hex[u[i] >> 4];
    s += hex[u[i] & 0xF];
  }
  return s;
}

}  // namespace proto
