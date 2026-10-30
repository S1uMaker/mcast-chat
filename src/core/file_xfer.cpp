#include "file_xfer.hpp"
#include "../app.hpp"
#include <mmsystem.h>
#include <algorithm>
#include <rpc.h>
#include "../crypto/sha256.hpp"
#include "../util/log.hpp"
#include "../util/text.hpp"

namespace core {

namespace {

constexpr int kMaxRetxRounds = 5;        // NACK 重传最多轮数
constexpr uint32_t kIntervalMs = 2;      // 节拍间隔（≈500 包/秒，timeBeginPeriod 下 Sleep 精度足够）
constexpr uint64_t kRecvTimeoutMs = 30000;      // 接收无活动超时
constexpr size_t kMaxRangesPerNack = 160;       // 单个 NACK 报文最多携带的缺失区间数

uint64_t now_tick() { return GetTickCount64(); }

std::string basename_of(const std::string& path) {
  size_t p = path.find_last_of("/\\");
  std::string name = (p == std::string::npos) ? path : path.substr(p + 1);
  if (name.empty()) name = "未命名文件";
  return name;
}

void random_id(uint8_t out[16]) {
  UUID u;
  if (UuidCreate(&u) == RPC_S_OK) {
    memcpy(out, &u, 16);  // UUID 结构恰为 16 字节
    return;
  }
  uint64_t t = GetTickCount64();
  for (int i = 0; i < 16; ++i) out[i] = static_cast<uint8_t>(t >> ((i % 8) * 8));
}

// 位图 → 缺失区间列表 [start, end] 闭区间
std::vector<std::pair<uint32_t, uint32_t>> missing_ranges(const std::vector<uint8_t>& bits,
                                                          uint32_t total) {
  std::vector<std::pair<uint32_t, uint32_t>> out;
  uint32_t runStart = UINT32_MAX;
  for (uint32_t i = 0; i < total; ++i) {
    bool have = (bits[i >> 3] & (1 << (i & 7))) != 0;
    if (!have && runStart == UINT32_MAX) runStart = i;
    if (have && runStart != UINT32_MAX) {
      out.push_back({runStart, i - 1});
      runStart = UINT32_MAX;
    }
  }
  if (runStart != UINT32_MAX) out.push_back({runStart, total - 1});
  return out;
}

// 目标名冲突时追加 " (1)" " (2)"…
std::wstring unique_target_name(const std::wstring& name) {
  std::wstring target = L"received\\" + name;
  if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) return name;
  size_t dot = name.find_last_of(L'.');
  std::wstring stem = (dot == std::wstring::npos) ? name : name.substr(0, dot);
  std::wstring ext = (dot == std::wstring::npos) ? L"" : name.substr(dot);
  for (int i = 1;; ++i) {
    std::wstring cand = stem + L" (" + std::to_wstring(i) + L")" + ext;
    if (GetFileAttributesW((L"received\\" + cand).c_str()) == INVALID_FILE_ATTRIBUTES) return cand;
  }
}

}  // namespace

FileXfer& filexfer() {
  static FileXfer mgr;
  return mgr;
}

// ---------------- 通用 ----------------

void FileXfer::send_file_msg(proto::MsgType type, const std::vector<uint8_t>& payload) {
  proto::Header h;
  h.type = type;
  h.sender = app().uuid;
  h.seq = app().seq.fetch_add(1);
  uint16_t flags = 0;  // M4：加密开启且持有组密钥时 AES-GCM 加密载荷
  std::string plain(reinterpret_cast<const char*>(payload.data()), payload.size());
  std::string body = crypto::session().seal_payload(type, app().uuid, h.seq, plain, flags, &sendKey_);
  h.flags = flags;
  std::vector<uint8_t> fin(body.begin(), body.end());
  auto pkt = proto::encode_packet(h, fin);
  app().sock.send(pkt.data(), pkt.size());
}

void FileXfer::push_event(const XferEvent& e) {
  std::lock_guard<std::mutex> g(evMtx_);
  events_.push_back(e);
}

std::vector<XferEvent> FileXfer::drain_events() {
  std::vector<XferEvent> out;
  std::lock_guard<std::mutex> g(evMtx_);
  out.swap(events_);
  return out;
}

bool FileXfer::sleep_cancellable(uint32_t ms) {
  uint32_t waited = 0;
  while (waited < ms) {
    if (sendStop_.load()) return false;
    Sleep(ms - waited > 50 ? 50 : ms - waited);
    waited += 50;
  }
  return !sendStop_.load();
}

// ---------------- 发送 ----------------

bool FileXfer::start_send(const std::string& utf8Path, std::string* err) {
  if (sending_.load()) {
    if (err) *err = "已有文件正在发送";
    return false;
  }
  if (senderThread_.joinable()) senderThread_.join();

  FILE* f = _wfopen(util::utf8_to_wide(utf8Path).c_str(), L"rb");
  if (!f) {
    if (err) *err = "无法打开文件";
    return false;
  }
  _fseeki64(f, 0, SEEK_END);
  int64_t sz = _ftelli64(f);
  fclose(f);
  if (sz <= 0) {
    if (err) *err = "空文件无法传输";
    return false;
  }

  CreateDirectoryW(L"received", nullptr);
  random_id(sendFileId_.data());
  sendName_ = basename_of(utf8Path);
  {
    std::lock_guard<std::mutex> g(retxMtx_);
    retxQueue_.clear();
    retxQueued_.assign(((static_cast<uint64_t>(sz) + proto::kFileChunkSize - 1) /
                        proto::kFileChunkSize + 7) / 8, 0);
  }
  sendTotal_ = static_cast<uint64_t>((sz + proto::kFileChunkSize - 1) / proto::kFileChunkSize);
  sendDone_ = 0;
  retxRound_ = 0;
  sendStop_ = false;
  sendFinished_ = false;
  sending_ = true;
  sendKey_ = crypto::session().current_key();  // M4：整份文件用同一把密钥快照
  push_event({XferEvent::SendStarted, sendName_, "", "", {}});

  uint64_t size = static_cast<uint64_t>(sz);
  senderThread_ = std::thread([this, path = utf8Path, size] { sender_thread(path, size); });
  return true;
}

void FileXfer::sender_thread(std::string path, uint64_t size) {
  timeBeginPeriod(1);  // 提高 Sleep 精度，保证节拍

  const uint32_t total =
      static_cast<uint32_t>((size + proto::kFileChunkSize - 1) / proto::kFileChunkSize);
  const std::string name = sendName_;

  FILE* f = nullptr;
  do {
    std::string sha = crypto::sha256_file(path, nullptr);
    if (sha.size() != 32) {
      push_event({XferEvent::SendFailed, name, "计算 SHA-256 失败", "", {}});
      break;
    }
    f = _wfopen(util::utf8_to_wide(path).c_str(), L"rb");
    if (!f) {
      push_event({XferEvent::SendFailed, name, "打开文件失败", "", {}});
      break;
    }

    // META = fileId + 名称 + 大小 + SHA-256 + 分块参数；开头连发 3 次抗丢包
    std::vector<uint8_t> meta;
    {
      proto::Writer w(meta);
      w.bytes(sendFileId_.data(), 16);
      w.utf8(name);
      w.u64(size);
      w.bytes(sha.data(), 32);
      w.u32(proto::kFileChunkSize);
      w.u32(total);
    }
    send_file_msg(proto::MSG_FILE_META, meta);
    if (!sleep_cancellable(400)) break;
    send_file_msg(proto::MSG_FILE_META, meta);
    if (!sleep_cancellable(400)) break;
    send_file_msg(proto::MSG_FILE_META, meta);

    // 第一遍：按节拍发送全部分块；期间周期性重发 META（晚加入者可中途接入）
    uint64_t lastMetaTick = GetTickCount64();
    std::vector<uint8_t> chunk(proto::kFileChunkSize);
    bool aborted = false;
    for (uint32_t seq = 0; seq < total; ++seq) {
      if (sendStop_.load()) { aborted = true; break; }
      _fseeki64(f, static_cast<int64_t>(seq) * proto::kFileChunkSize, SEEK_SET);
      size_t n = fread(chunk.data(), 1, proto::kFileChunkSize, f);
      if (n == 0) {
        push_event({XferEvent::SendFailed, name, "读取文件失败", "", {}});
        send_cancel(sendFileId_.data(), "发送方读取文件失败");
        aborted = true;
        break;
      }
      std::vector<uint8_t> p;
      proto::Writer w(p);
      w.bytes(sendFileId_.data(), 16);
      w.u32(seq);
      w.bytes(chunk.data(), n);
      send_file_msg(proto::MSG_FILE_DATA, p);
      sendDone_.store(seq + 1);

      if (GetTickCount64() - lastMetaTick > 5000) {
        send_file_msg(proto::MSG_FILE_META, meta);
        lastMetaTick = GetTickCount64();
      }
      Sleep(kIntervalMs);
    }
    if (aborted) break;

    auto end_payload = [&] {
      std::vector<uint8_t> p;
      proto::Writer w(p);
      w.bytes(sendFileId_.data(), 16);
      return p;
    };
    send_file_msg(proto::MSG_FILE_END, end_payload());
    Sleep(200);
    send_file_msg(proto::MSG_FILE_END, end_payload());

    // 重传轮次：等窗口收集 NACK → 只重传缺失块 → 再发 END 让接收方复查
    bool stopped = false, allDone = false;
    for (int round = 1; round <= kMaxRetxRounds && !stopped; ++round) {
      if (!sleep_cancellable(2000)) { stopped = true; break; }
      std::vector<uint32_t> list;
      {
        std::lock_guard<std::mutex> g(retxMtx_);
        list.swap(retxQueue_);
        std::fill(retxQueued_.begin(), retxQueued_.end(), 0);
      }
      if (list.empty()) { allDone = true; break; }
      retxRound_.store(round);
      util::dlog("xfer: retx round %d, %u chunks", round, static_cast<unsigned>(list.size()));
      for (uint32_t seq : list) {
        if (sendStop_.load()) { stopped = true; break; }
        if (seq >= total) continue;
        _fseeki64(f, static_cast<int64_t>(seq) * proto::kFileChunkSize, SEEK_SET);
        size_t n = fread(chunk.data(), 1, proto::kFileChunkSize, f);
        if (n == 0) continue;
        std::vector<uint8_t> p;
        proto::Writer w(p);
        w.bytes(sendFileId_.data(), 16);
        w.u32(seq);
        w.bytes(chunk.data(), n);
        send_file_msg(proto::MSG_FILE_DATA, p);
        Sleep(kIntervalMs);
      }
      if (stopped) break;
      send_file_msg(proto::MSG_FILE_END, end_payload());
      Sleep(200);
      send_file_msg(proto::MSG_FILE_END, end_payload());
    }
    if (stopped) break;

    if (allDone) {
      push_event({XferEvent::SendDone, name, "", "", {}});
    } else {
      push_event({XferEvent::SendFailed, name, "接收方重传 5 轮仍未收全", "", {}});
      send_cancel(sendFileId_.data(), "多次重传未收全，取消传输");
    }
  } while (false);

  if (f) fclose(f);
  sending_ = false;
  retxRound_ = 0;
  timeEndPeriod(1);
  sendFinished_ = true;  // 最后置位，tick 据此 join
}

void FileXfer::send_cancel(const uint8_t* fileId, const char* reason) {
  std::vector<uint8_t> p;
  proto::Writer w(p);
  w.bytes(fileId, 16);
  w.utf8(reason);
  send_file_msg(proto::MSG_FILE_CANCEL, p);
}

// ---------------- 接收 ----------------

void FileXfer::start_recv(const proto::Header& h, const std::string& name, uint64_t size,
                          const std::string& sha, uint32_t chunkSize, uint32_t total) {
  CreateDirectoryW(L"received", nullptr);
  recvName_ = name;
  recvSize_ = size;
  recvSha_ = sha;
  recvChunkSize_ = chunkSize;
  recvTotal_ = total;
  recvGot_ = 0;
  recvNackRounds_ = 0;
  recvEvalAt_ = 0;
  recvLastAct_ = now_tick();
  recvBitmap_.assign((total + 7) / 8, 0);
  testDroppedOnce_.assign((total + 7) / 8, 0);
  recvTmpPath_ = std::string("received/") + proto::uuid_to_string(recvFileId_) + ".part";
  recvFile_ = _wfopen(util::utf8_to_wide(recvTmpPath_).c_str(), L"wb");
  if (!recvFile_) {
    push_event({XferEvent::RecvFailed, name, "无法创建临时文件", "", {}});
    return;
  }
  receiving_ = true;
  push_event({XferEvent::RecvStarted, name, "", "", h.sender});
  util::dlog("xfer: recv start %s (%.1f MB, %u chunks)", name.c_str(), size / 1048576.0, total);
}

void FileXfer::on_packet(const proto::Header& h, proto::Reader& r, uint32_t fromIp) {
  (void)fromIp;
  switch (h.type) {
    case proto::MSG_FILE_META: {
      uint8_t fid[16];
      std::string name, sha(32, '\0');
      uint64_t size = 0;
      uint32_t chunkSize = 0, total = 0;
      if (!(r.bytes(fid, 16) && r.utf8(name) && r.u64(size) &&
            r.bytes(reinterpret_cast<uint8_t*>(&sha[0]), 32) && r.u32(chunkSize) &&
            r.u32(total) && r.ok()))
        return;
      if (receiving_.load()) {
        if (memcmp(fid, recvFileId_.data(), 16) == 0) return;  // 重复 META
        util::dlog("xfer: busy, ignore meta for %s", name.c_str());
        return;  // M3 简化：同一时刻只收一个文件
      }
      if (size == 0 || total == 0 || chunkSize == 0 || chunkSize > 1300) return;
      memcpy(recvFileId_.data(), fid, 16);
      start_recv(h, basename_of(name), size, sha, chunkSize, total);
      break;
    }
    case proto::MSG_FILE_DATA: {
      uint8_t fid[16];
      uint32_t seq = 0;
      if (!(r.bytes(fid, 16) && r.u32(seq))) return;
      if (!receiving_.load() || memcmp(fid, recvFileId_.data(), 16) != 0) return;
      if (seq >= recvTotal_) return;
      recvLastAct_ = now_tick();
      if (recvBitmap_[seq >> 3] & (1 << (seq & 7))) return;  // 重复块
      // 测试钩子：模拟真实丢包——每个块只丢第一次，重传放行
      if (app().testDropEvery > 0 && seq % static_cast<uint32_t>(app().testDropEvery) == 0 &&
          !(testDroppedOnce_[seq >> 3] & (1 << (seq & 7)))) {
        testDroppedOnce_[seq >> 3] |= static_cast<uint8_t>(1 << (seq & 7));
        util::dlog("xfer: TEST drop chunk %u (once)", seq);
        return;
      }
      size_t dataLen = r.remaining();
      if (dataLen == 0 || dataLen > proto::kFileChunkSize) return;
      uint8_t buf[proto::kFileChunkSize];
      if (!r.bytes(buf, dataLen)) return;
      _fseeki64(recvFile_, static_cast<int64_t>(seq) * recvChunkSize_, SEEK_SET);
      fwrite(buf, 1, dataLen, recvFile_);
      recvBitmap_[seq >> 3] |= static_cast<uint8_t>(1 << (seq & 7));
      ++recvGot_;
      break;
    }
    case proto::MSG_FILE_END: {
      uint8_t fid[16];
      if (!r.bytes(fid, 16)) return;
      if (!receiving_.load() || memcmp(fid, recvFileId_.data(), 16) != 0) return;
      recvLastAct_ = now_tick();
      recvEvalAt_ = now_tick() + 800;  // 等在途分块落地后复查
      break;
    }
    case proto::MSG_NACK: {
      uint8_t fid[16];
      uint32_t count = 0;
      if (!(r.bytes(fid, 16) && r.u32(count))) return;
      if (!sending_.load() || memcmp(fid, sendFileId_.data(), 16) != 0) return;
      std::lock_guard<std::mutex> g(retxMtx_);
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t s = 0, e = 0;
        if (!(r.u32(s) && r.u32(e))) return;
        uint32_t total = static_cast<uint32_t>(sendTotal_.load());
        if (e < s || e >= total) continue;
        for (uint32_t q = s; q <= e; ++q) {
          if (!(retxQueued_[q >> 3] & (1 << (q & 7)))) {
            retxQueued_[q >> 3] |= static_cast<uint8_t>(1 << (q & 7));
            retxQueue_.push_back(q);
          }
        }
      }
      break;
    }
    case proto::MSG_FILE_CANCEL: {
      uint8_t fid[16];
      std::string reason;
      if (!(r.bytes(fid, 16) && r.utf8(reason))) return;
      if (!receiving_.load() || memcmp(fid, recvFileId_.data(), 16) != 0) return;
      recv_fail("发送方取消：" + reason);
      break;
    }
    default:
      break;
  }
}

void FileXfer::recv_evaluate() {
  auto missing = missing_ranges(recvBitmap_, recvTotal_);
  if (missing.empty()) {
    // 收全：关闭 → SHA-256 校验 → 落盘改名
    fclose(recvFile_);
    recvFile_ = nullptr;
    std::string sha = crypto::sha256_file(recvTmpPath_, nullptr);
    if (sha.size() != 32 || sha != recvSha_) {
      _wremove(util::utf8_to_wide(recvTmpPath_).c_str());
      push_event({XferEvent::RecvFailed, recvName_, "SHA-256 校验不一致", "", {}});
      util::dlog("xfer: sha mismatch for %s", recvName_.c_str());
    } else {
      std::wstring finalName = unique_target_name(util::utf8_to_wide(recvName_));
      std::wstring target = L"received\\" + finalName;
      if (MoveFileExW(util::utf8_to_wide(recvTmpPath_).c_str(), target.c_str(),
                      MOVEFILE_COPY_ALLOWED)) {
        lastReceived_ = util::wide_to_utf8(target);
        push_event({XferEvent::RecvDone, recvName_, "", lastReceived_, {}});
        util::dlog("xfer: recv done -> %s", lastReceived_.c_str());
      } else {
        push_event({XferEvent::RecvFailed, recvName_, "保存文件失败", "", {}});
      }
    }
    recv_cleanup(false);
    return;
  }

  if (recvNackRounds_ < kMaxRetxRounds) {
    ++recvNackRounds_;
    uint32_t missingCount = 0;
    for (auto& rg : missing) missingCount += rg.second - rg.first + 1;
    for (size_t i = 0; i < missing.size();) {
      std::vector<uint8_t> p;
      proto::Writer w(p);
      w.bytes(recvFileId_.data(), 16);
      uint32_t n = static_cast<uint32_t>(std::min<size_t>(missing.size() - i, kMaxRangesPerNack));
      w.u32(n);
      for (uint32_t k = 0; k < n; ++k, ++i) {
        w.u32(missing[i].first);
        w.u32(missing[i].second);
      }
      send_file_msg(proto::MSG_NACK, p);
    }
    push_event({XferEvent::RecvNack, recvName_,
                "缺失 " + std::to_string(missingCount) + " 块，第 " +
                    std::to_string(recvNackRounds_) + " 轮请求重传",
                "", {}});
    recvEvalAt_ = now_tick() + 2500;  // 等重传到达后再次自查
    util::dlog("xfer: nack round %d, missing %u", recvNackRounds_, missingCount);
  } else {
    recv_fail("重传 5 轮仍未收全");
  }
}

void FileXfer::recv_fail(const std::string& why) {
  push_event({XferEvent::RecvFailed, recvName_, why, "", {}});
  util::dlog("xfer: recv fail: %s", why.c_str());
  recv_cleanup(true);
}

void FileXfer::recv_cleanup(bool removeTmp) {
  if (recvFile_) {
    fclose(recvFile_);
    recvFile_ = nullptr;
  }
  if (removeTmp && !recvTmpPath_.empty())
    _wremove(util::utf8_to_wide(recvTmpPath_).c_str());
  receiving_ = false;
  recvEvalAt_ = 0;
  recvGot_ = 0;
  recvNackRounds_ = 0;
}

// ---------------- 节拍 / 退出 ----------------

void FileXfer::tick() {
  if (sendFinished_.load() && senderThread_.joinable()) senderThread_.join();
  if (receiving_.load()) {
    uint64_t now = now_tick();
    if (recvEvalAt_ != 0 && now >= recvEvalAt_) {
      recvEvalAt_ = 0;
      recv_evaluate();
      return;
    }
    if (now - recvLastAct_ > kRecvTimeoutMs) recv_fail("接收超时，发送方可能已中断");
  }
}

void FileXfer::shutdown() {
  sendStop_ = true;
  if (senderThread_.joinable()) senderThread_.join();
  if (recvFile_) {  // 保留 .part 以便排查，只关句柄
    fclose(recvFile_);
    recvFile_ = nullptr;
  }
}

}  // namespace core
