#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace proto {

// 大端序序列化助手（写）。不直接 cast struct：避开 padding/对齐/字节序陷阱。
class Writer {
 public:
  explicit Writer(std::vector<uint8_t>& buf) : buf_(buf) {}
  void u8(uint8_t v) { buf_.push_back(v); }
  void u16(uint16_t v);
  void u32(uint32_t v);
  void u64(uint64_t v);
  void bytes(const void* p, size_t n);
  void utf8(const std::string& s);  // u16 长度 + 字节（≤65535）

 private:
  std::vector<uint8_t>& buf_;
};

// 大端序序列化助手（读），全程带边界检查，越界后整体置为失败。
class Reader {
 public:
  Reader(const uint8_t* p, size_t n) : p_(p), left_(n) {}
  bool u8(uint8_t& v);
  bool u16(uint16_t& v);
  bool u32(uint32_t& v);
  bool u64(uint64_t& v);
  bool bytes(uint8_t* out, size_t n);
  bool utf8(std::string& s);
  bool ok() const { return good_; }
  size_t remaining() const { return left_; }

 private:
  const uint8_t* p_;
  size_t left_;
  bool good_ = true;
};

}  // namespace proto
