#pragma once
// 线上协议：32 字节定长头 + 载荷（网络字节序 / 大端）
// 协议明细见 ARCHITECTURE.md 第 5 节
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace proto {

constexpr uint32_t kMagic = 0x4D434854u;  // 'MCHT'
constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderSize = 32;
constexpr size_t kMaxDgram = 1400;      // 以太网 MTU 内，避免 IP 分片放大丢包
constexpr size_t kFileChunkSize = 1024; // 文件分块大小（M3 使用）

// 加 MSG_ 前缀：windows.h 定义了 FILE_END 等同名宏
enum MsgType : uint8_t {
  MSG_HELLO       = 0x01,
  MSG_HELLO_ACK   = 0x02,
  MSG_HEARTBEAT   = 0x03,
  MSG_BYE         = 0x04,
  MSG_KICK        = 0x05,
  MSG_KEY_DIST    = 0x06,
  MSG_CHAT_TEXT   = 0x10,
  MSG_FILE_META   = 0x20,
  MSG_FILE_DATA   = 0x21,
  MSG_FILE_END    = 0x22,
  MSG_NACK        = 0x23,
  MSG_FILE_CANCEL = 0x24,
};

enum HeaderFlags : uint16_t {
  FLAG_ENCRYPTED = 0x0001,  // M4：载荷已用组密钥 AES-GCM 加密
  FLAG_SIGNED    = 0x0002,  // M4：载荷带 RSA 签名
};

using Uuid = std::array<uint8_t, 16>;  // 成员唯一标识（UUIDv4）

struct Header {
  uint8_t type = 0;
  uint16_t flags = 0;
  Uuid sender{};
  uint32_t seq = 0;  // 发送端单调递增
};

struct Packet {
  Header hdr;
  const uint8_t* payload = nullptr;  // 指向原缓冲区内部（不拷贝）
  size_t payload_len = 0;
};

// 头 + 载荷 序列化为一整个报文
std::vector<uint8_t> encode_packet(const Header& h, const std::vector<uint8_t>& payload);

// 解析报文；失败（残包/魔数不符/长度不符）返回 false
bool parse_packet(const uint8_t* data, size_t len, Packet& out);

std::string uuid_to_string(const Uuid& u);  // 调试用

}  // namespace proto
