#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "../protocol/codec.hpp"
#include "../protocol/message.hpp"

namespace core {

struct Member {
  proto::Uuid id{};
  std::string name;       // UTF-8
  uint32_t ip = 0;        // 网络字节序
  uint64_t joinTime = 0;  // epoch 毫秒（跨机可比，用于选举房主）
  uint64_t lastSeen = 0;  // 本地 GetTickCount64
  bool online = true;
  bool self = false;
};

// 成员表变化事件（UI 线程消费：聊天区系统消息 + 成员列表刷新）
struct MemberEvent {
  enum Kind : int { Joined, Left, Kicked, Offline, Online, HostChanged, KickedSelf };
  Kind kind;
  std::string name;  // 事件主体昵称（KickedSelf 时为房主昵称）
};

// 多播组成员管理（应用层成员表）
// 组播的组成员关系在 IP/IGMP 层，应用不可查询，因此必须靠协议自建：
//   HELLO/HELLO_ACK 握手建表 → HEARTBEAT 3s 保活 → 超时/离开/踢人移除
// 所有方法只在 UI 线程调用（接收报文经 PostMessage 投递，天然串行）。
class MemberManager {
 public:
  void init_self();  // 启动登记自己（需在创建窗口前调用，首次刷新可见）
  void announce();   // 发送首轮 HELLO（应在接收线程启动后调用）
  void shutdown();   // 发送 BYE
  void tick();       // 1s 节拍：HELLO 补发 / 3s 心跳 / 超时扫描

  void on_packet(const proto::Header& h, proto::Reader& r, uint32_t fromIp);

  std::vector<Member> snapshot() const;  // 按 joinTime 排序（房主在前）
  std::vector<MemberEvent> drain_events();
  bool take_dirty();                     // 成员表有展示相关变化（供 UI 刷新）
  bool is_host() const;
  proto::Uuid host_id() const;
  size_t count() const { return list_.size(); }

  bool kick(const proto::Uuid& target, std::string* err);  // 仅房主有效

 private:
  void send_msg(proto::MsgType type, const std::vector<uint8_t>& payload);
  void send_hello();
  void send_hello_ack(const proto::Uuid& target);
  void send_heartbeat();
  void send_bye();

  Member* find(const proto::Uuid& id);
  bool blocked(const proto::Uuid& id) const;
  // announce=true 时对“新成员”发 Joined 事件（HELLO 场景）；
  // ACK/心跳等被动学习只静默入表，避免新成员一进来就刷屏
  void upsert(const proto::Uuid& id, const std::string& name, uint64_t joinTime,
              uint32_t ip, bool announce);
  void remove(const proto::Uuid& id, bool kicked);
  void apply_kick(const proto::Uuid& target);
  void recompute_host();
  std::string host_name() const;

  std::vector<Member> list_;
  std::vector<MemberEvent> events_;
  std::vector<proto::Uuid> blocked_;  // 被房主踢出的成员（忽略其任何报文）
  proto::Uuid hostId_{};
  bool hostValid_ = false;
  bool dirty_ = false;
  uint64_t joinTime_ = 0;   // 自己的 joinTime（HELLO 载荷用）
  uint32_t tickCount_ = 0;
  int helloSends_ = 0;      // 后续待补发的 HELLO 次数
};

MemberManager& members();  // 全局单例

}  // namespace core
