#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../protocol/codec.hpp"
#include "../protocol/message.hpp"

namespace core {

// 文件传输事件（UI 线程消费：聊天区系统消息 / 播放按钮 / 进度条复位）
struct XferEvent {
  enum Kind : int { SendStarted, SendDone, SendFailed, RecvStarted, RecvNack, RecvDone, RecvFailed };
  Kind kind;
  std::string name;      // 文件名
  std::string detail;    // 附加说明（失败原因 / 重传轮次等）
  std::string path;      // RecvDone：保存路径（播放按钮用）
  proto::Uuid sender{};  // RecvStarted：发送方（解析昵称用）
};

// 多播文件传输（M3）。UDP 多播不可靠，可靠性自己补：
//   分块(1024B) + 节拍发送(默认 500 包/秒) + 接收位图 + NACK 区间请求重传(≤5 轮)
//   + SHA-256 完整性校验 + 周期重发 META（晚加入者可中途接入）。
// 发送在专用线程做节拍循环；接收报文处理在 UI 线程（on_packet，含落盘）。
class FileXfer {
 public:
  void shutdown();  // 停发送线程（WM_DESTROY 调用）
  void tick();      // UI 1s 节拍：超时 / 延迟评估 / 收全判定
  void on_packet(const proto::Header& h, proto::Reader& r, uint32_t fromIp);

  bool start_send(const std::string& utf8Path, std::string* err);

  bool sending() const { return sending_.load(); }
  bool receiving() const { return receiving_.load(); }
  uint64_t send_done() const { return sendDone_.load(); }
  uint64_t send_total() const { return sendTotal_.load(); }
  uint64_t recv_done() const { return recvGot_; }    // 接收侧全在 UI 线程，无需原子
  uint64_t recv_total() const { return recvTotal_; }
  int retx_round() const { return retxRound_.load(); }
  const std::string& send_name() const { return sendName_; }
  const std::string& recv_name() const { return recvName_; }
  const std::string& last_received() const { return lastReceived_; }

  std::vector<XferEvent> drain_events();

 private:
  // ---------- 发送（sender 线程） ----------
  void sender_thread(std::string path, uint64_t size);
  bool sleep_cancellable(uint32_t ms);
  void send_cancel(const uint8_t* fileId, const char* reason);

  // ---------- 接收（UI 线程） ----------
  void start_recv(const proto::Header& h, const std::string& name, uint64_t size,
                  const std::string& sha, uint32_t chunkSize, uint32_t totalChunks);
  void recv_evaluate();               // 收全判定 / 发 NACK / 放弃
  void recv_fail(const std::string& why);
  void recv_cleanup(bool removeTmp);

  void send_file_msg(proto::MsgType type, const std::vector<uint8_t>& payload);
  void push_event(const XferEvent& e);

  // ---------- 发送上下文 ----------
  std::thread senderThread_;
  std::atomic<bool> sendStop_{false};
  std::atomic<bool> sendFinished_{false};
  std::atomic<bool> sending_{false};
  std::atomic<uint64_t> sendDone_{0}, sendTotal_{0};
  std::atomic<int> retxRound_{0};
  proto::Uuid sendFileId_{};
  std::string sendName_;
  std::mutex retxMtx_;
  std::vector<uint32_t> retxQueue_;   // 待重传块号（接收方 NACK 排入）
  std::vector<uint8_t> retxQueued_;   // 位图：已排队防重复

  // ---------- 接收上下文（仅 UI 线程访问） ----------
  std::atomic<bool> receiving_{false};
  proto::Uuid recvFileId_{};
  std::string recvName_, recvTmpPath_, recvSha_;
  uint64_t recvSize_ = 0, recvLastAct_ = 0, recvEvalAt_ = 0;
  uint32_t recvChunkSize_ = 0, recvTotal_ = 0, recvGot_ = 0;
  int recvNackRounds_ = 0;
  std::vector<uint8_t> recvBitmap_;      // 已收块位图
  std::vector<uint8_t> testDroppedOnce_; // 测试钩子：每块只丢一次（模拟真实丢包）
  FILE* recvFile_ = nullptr;

  std::string lastReceived_;
  std::mutex evMtx_;
  std::vector<XferEvent> events_;
};

FileXfer& filexfer();  // 全局单例

}  // namespace core
