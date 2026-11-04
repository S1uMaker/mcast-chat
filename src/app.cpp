#include "app.hpp"
#include <algorithm>
#include <rpc.h>

static App g_app;

App& app() { return g_app; }

static int hex_nib(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

bool App::init(const std::string& userName) {
  name = userName;

  // UUIDv4 → 16 字节成员标识（rpcrt4）
  UUID u;
  if (UuidCreate(&u) != RPC_S_OK) return false;
  unsigned char* str = nullptr;
  if (UuidToStringA(&u, &str) != RPC_S_OK) return false;
  std::string hex(reinterpret_cast<char*>(str));
  RpcStringFreeA(&str);

  hex.erase(std::remove(hex.begin(), hex.end(), '-'), hex.end());
  for (int i = 0; i < 16 && 2 * i + 1 < static_cast<int>(hex.size()); ++i)
    uuid[i] = static_cast<uint8_t>((hex_nib(hex[2 * i]) << 4) | hex_nib(hex[2 * i + 1]));
  return true;
}
