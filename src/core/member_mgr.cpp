#include "member_mgr.hpp"
#include <algorithm>
#include <chrono>
#include "../app.hpp"
#include "../util/log.hpp"

namespace core {

namespace {

uint64_t now_wall_ms() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

uint64_t now_tick() { return GetTickCount64(); }

bool read_blob(proto::Reader& r, std::string& out) {
  uint16_t n = 0;
  if (!r.u16(n)) return false;
  out.assign(n, '\0');
  if (n > 0 && !r.bytes(reinterpret_cast<uint8_t*>(&out[0]), n)) return false;
  return true;
}

}  // namespace

MemberManager& members() {
  static MemberManager mgr;
  return mgr;
}

void MemberManager::init_self() {
  Member me;
  me.id = app().uuid;
  me.name = app().name;
  me.joinTime = now_wall_ms();
  me.lastSeen = now_tick();
  me.online = true;
  me.self = true;
  joinTime_ = me.joinTime;
  list_.push_back(me);
  recompute_host();
  dirty_ = true;
  util::dlog("member: self registered, joinTime=%llu", static_cast<unsigned long long>(joinTime_));
}

void MemberManager::announce() {
  send_hello();
  helloSends_ = 2;  // tick 中再补发 2 次（抗首个 HELLO 丢包）
}

void MemberManager::shutdown() { send_bye(); }

// ---------------- 发送 ----------------

void MemberManager::send_msg(proto::MsgType type, const std::vector<uint8_t>& payload) {
  proto::Header h;
  h.type = type;
  h.flags = 0;
  h.sender = app().uuid;
  h.seq = app().seq.fetch_add(1);
  auto pkt = proto::encode_packet(h, payload);
  std::string err;
  if (!app().sock.send(pkt.data(), pkt.size(), &err))
    util::dlog("member: send type=0x%02x failed: %s", type, err.c_str());
}

void MemberManager::send_hello() {
  std::vector<uint8_t> p;
  proto::Writer w(p);
  w.utf8(app().name);
  w.u64(joinTime_);
  w.u16(0);  // 公钥 blob 长度（M4 填充 RSA 公钥）
  send_msg(proto::MSG_HELLO, p);
}

void MemberManager::send_hello_ack(const proto::Uuid& target) {
  std::vector<uint8_t> p;
  proto::Writer w(p);
  w.bytes(target.data(), target.size());
  w.utf8(app().name);
  w.u64(joinTime_);
  w.u16(0);  // M4
  send_msg(proto::MSG_HELLO_ACK, p);
}

void MemberManager::send_heartbeat() {
  std::vector<uint8_t> p;
  proto::Writer w(p);
  w.utf8(app().name);
  w.u64(joinTime_);
  send_msg(proto::MSG_HEARTBEAT, p);
}

void MemberManager::send_bye() { send_msg(proto::MSG_BYE, {}); }

// ---------------- 成员表 ----------------

Member* MemberManager::find(const proto::Uuid& id) {
  for (auto& m : list_)
    if (m.id == id) return &m;
  return nullptr;
}

bool MemberManager::blocked(const proto::Uuid& id) const {
  for (const auto& b : blocked_)
    if (b == id) return true;
  return false;
}

void MemberManager::upsert(const proto::Uuid& id, const std::string& name, uint64_t joinTime,
                           uint32_t ip, bool announce) {
  if (id == app().uuid || blocked(id)) return;

  Member* m = find(id);
  if (!m) {
    Member nm;
    nm.id = id;
    nm.name = name.empty() ? "(未命名)" : name;
    nm.joinTime = joinTime ? joinTime : now_wall_ms();
    nm.lastSeen = now_tick();
    nm.ip = ip;
    nm.online = true;
    list_.push_back(nm);
    if (announce) events_.push_back({MemberEvent::Joined, nm.name});
    dirty_ = true;
    recompute_host();
    util::dlog("member: +%s (%zu online)", nm.name.c_str(), list_.size());
    return;
  }

  bool changed = false;
  if (!name.empty() && m->name != name) { m->name = name; changed = true; }
  if (m->ip != ip) { m->ip = ip; changed = true; }
  m->lastSeen = now_tick();
  if (!m->online) {  // 超时离线后恢复通信
    m->online = true;
    events_.push_back({MemberEvent::Online, m->name});
    changed = true;
  }
  if (changed) dirty_ = true;
}

void MemberManager::remove(const proto::Uuid& id, bool kicked) {
  for (auto it = list_.begin(); it != list_.end(); ++it) {
    if (it->id != id) continue;
    if (!it->self) events_.push_back({kicked ? MemberEvent::Kicked : MemberEvent::Left, it->name});
    util::dlog("member: -%s (kicked=%d)", it->name.c_str(), (int)kicked);
    list_.erase(it);
    dirty_ = true;
    recompute_host();
    return;
  }
}

void MemberManager::recompute_host() {
  const Member* best = nullptr;
  for (const auto& m : list_) {
    if (!m.online) continue;
    if (!best || m.joinTime < best->joinTime ||
        (m.joinTime == best->joinTime && m.id < best->id))
      best = &m;
  }
  if (!best) {
    hostValid_ = false;
    return;
  }
  bool changed = !hostValid_ || hostId_ != best->id;
  hostId_ = best->id;
  hostValid_ = true;
  if (changed) {
    dirty_ = true;
    if (list_.size() > 1 || !best->self)  // 单机独自入组时不刷“成为房主”
      events_.push_back({MemberEvent::HostChanged, best->name});
    util::dlog("member: host -> %s", best->name.c_str());
  }
}

std::string MemberManager::host_name() const {
  for (const auto& m : list_)
    if (m.id == hostId_) return m.name;
  return std::string();
}

// ---------------- 收报文 ----------------

void MemberManager::on_packet(const proto::Header& h, proto::Reader& r, uint32_t fromIp) {
  if (h.sender == app().uuid) return;  // 自己的回环包
  if (blocked(h.sender)) return;

  switch (h.type) {
    case proto::MSG_HELLO: {
      std::string name, pk;
      uint64_t jt = 0;
      if (!(r.utf8(name) && r.u64(jt) && read_blob(r, pk) && r.ok())) return;
      upsert(h.sender, name, jt, fromIp, /*announce=*/true);
      send_hello_ack(h.sender);  // 每个 HELLO 都回 ACK（重复到达也幂等）
      break;
    }
    case proto::MSG_HELLO_ACK: {
      proto::Uuid target{};
      if (!r.bytes(target.data(), target.size())) return;
      if (target != app().uuid) return;  // 不是发给我的
      std::string name, pk;
      uint64_t jt = 0;
      if (!(r.utf8(name) && r.u64(jt) && read_blob(r, pk) && r.ok())) return;
      upsert(h.sender, name, jt, fromIp, /*announce=*/false);
      break;
    }
    case proto::MSG_HEARTBEAT: {
      std::string name;
      uint64_t jt = 0;
      if (!(r.utf8(name) && r.u64(jt) && r.ok())) return;
      upsert(h.sender, name, jt, fromIp, /*announce=*/false);
      break;
    }
    case proto::MSG_BYE: {
      remove(h.sender, /*kicked=*/false);
      break;
    }
    case proto::MSG_KICK: {
      // M2：只信“我认为的房主”发来的 KICK（M4 再叠加 RSA 签名验证）
      if (!hostValid_ || h.sender != hostId_) {
        util::dlog("member: ignore KICK from non-host");
        return;
      }
      proto::Uuid target{};
      std::string sig;
      if (!r.bytes(target.data(), target.size()) || !read_blob(r, sig) || !r.ok()) return;
      apply_kick(target);
      break;
    }
    default:
      break;
  }
}

// ---------------- 踢人 ----------------

bool MemberManager::kick(const proto::Uuid& target, std::string* err) {
  if (!is_host()) {
    if (err) *err = "只有房主可以踢人";
    return false;
  }
  if (target == app().uuid) {
    if (err) *err = "不能踢自己";
    return false;
  }
  if (!find(target)) {
    if (err) *err = "该成员不在列表中";
    return false;
  }
  std::vector<uint8_t> p;
  proto::Writer w(p);
  w.bytes(target.data(), target.size());
  w.u16(0);  // M4：房主 RSA 私钥签名
  send_msg(proto::MSG_KICK, p);
  apply_kick(target);  // 本地同样生效（自己的回环包会被忽略）
  util::dlog("member: kicked %s", proto::uuid_to_string(target).c_str());
  return true;
}

void MemberManager::apply_kick(const proto::Uuid& target) {
  if (!blocked(target)) blocked_.push_back(target);
  if (find(target)) {
    if (target == app().uuid) {
      // 我是被踢者：事件里带上房主昵称供 UI 提示
      events_.push_back({MemberEvent::KickedSelf, host_name()});
      for (auto it = list_.begin(); it != list_.end(); ++it) {
        if (it->id == target) { list_.erase(it); break; }
      }
      dirty_ = true;
      recompute_host();
    } else {
      remove(target, /*kicked=*/true);
    }
  }
}

// ---------------- 节拍 ----------------

void MemberManager::tick() {
  ++tickCount_;
  if (helloSends_ > 0) {
    send_hello();
    --helloSends_;
  }
  if (tickCount_ % 3 == 0) send_heartbeat();

  const uint64_t now = now_tick();
  bool changed = false;
  for (size_t i = 0; i < list_.size();) {
    Member& m = list_[i];
    if (m.self) { ++i; continue; }
    uint64_t idle = now - m.lastSeen;
    if (m.online && idle > 10000) {
      m.online = false;
      events_.push_back({MemberEvent::Offline, m.name});
      changed = true;
      ++i;
    } else if (!m.online && idle > 20000) {
      events_.push_back({MemberEvent::Left, m.name});
      list_.erase(list_.begin() + static_cast<std::ptrdiff_t>(i));
      changed = true;
    } else {
      ++i;
    }
  }
  if (changed) {
    dirty_ = true;
    recompute_host();
  }
}

// ---------------- 查询 ----------------

std::vector<Member> MemberManager::snapshot() const {
  std::vector<Member> v = list_;
  std::sort(v.begin(), v.end(), [](const Member& a, const Member& b) {
    if (a.joinTime != b.joinTime) return a.joinTime < b.joinTime;
    return a.id < b.id;
  });
  return v;
}

std::vector<MemberEvent> MemberManager::drain_events() {
  std::vector<MemberEvent> out;
  out.swap(events_);
  return out;
}

bool MemberManager::take_dirty() {
  bool d = dirty_;
  dirty_ = false;
  return d;
}

bool MemberManager::is_host() const { return hostValid_ && hostId_ == app().uuid; }

proto::Uuid MemberManager::host_id() const { return hostId_; }

}  // namespace core
