# mcast-chat 架构与技术栈设计

带有文件传输功能的多播聊天工具 —— 网络编程课程大作业

> 核心约束：**不设任何中心服务器**，文字聊天、文件传输、成员管理、密钥分发全部
> 跑在一个 UDP 多播组上（做成"服务器聊天室"考核不合格）。

## 1. 技术栈结论

| 维度 | 选型 | 理由 |
|---|---|---|
| 语言 | C++17/20 | 课程底层风格；本机已有 MinGW-w64 GCC 15.2（`C:\mingw64`） |
| 网络 | Winsock2 UDP 多播 | `IP_ADD_MEMBERSHIP` 加组、`IP_MULTICAST_TTL/LOOP` |
| GUI | Win32 API 原生控件 | 零第三方依赖，MinGW 直接编译；聊天区/成员列表/输入框够用 |
| 加密 | Windows CNG（`bcrypt.h`） | RSA-2048 + AES-256-GCM + SHA-256 系统内置，免装 OpenSSL |
| 并发 | `std::thread` + `std::mutex`（收线程）+ `SetTimer`（心跳/节拍） | 本机工具链已验证可用 |
| 构建 | Makefile + build.bat（g++，链接 `ws2_32 bcrypt shell32 iphlpapi`） | 一条命令出 exe |

外部依赖为零：只用 Windows SDK 自带头文件，交作业/换机编译无负担。

## 2. 需求 → 设计映射

| 需求 | 实现 |
|---|---|
| a. 文本聊天 | `TEXT` 多播报文（可加密），UI 聊天区滚动显示 |
| b. 文件传输（音视频，接收后能播放） | `FILE_META/FILE_DATA/FILE_END` 分块多播 + `NACK` 选择重传 + SHA-256 校验；完成后存 `received/`，点"播放"调 `ShellExecute` 用系统默认播放器打开 |
| c. 成员管理 | **应用层成员表**：入组握手（HELLO/HELLO_ACK）建表 → 心跳保活 → 超时/BYE/踢人移除；UI 成员列表实时显示 |
| d. 非对称加密（加分） | 每人 RSA-2048 密钥对（公钥随 HELLO 广播）；房主生成 AES-256 组会话密钥，用各成员 RSA 公钥"信封分发"；此后聊天与文件载荷全部 AES-GCM 加密；附加密开关便于抓包对比 |

## 3. 总体架构

```
┌────────────────────────────────────────────────────────┐
│                 UI 层（Win32 API，单线程）               │
│  聊天区(ListView)  成员列表(ListView)  输入框+按钮  进度条 │
├────────────────────────────────────────────────────────┤
│                 应用控制层 (core/)                       │
│  ChatController   MemberManager    FileTransferManager  │
│  （消息分发）      （成员表/心跳/踢人）（发送/接收状态机）   │
├────────────────────────────────────────────────────────┤
│  安全模块 (crypto/)：RsaKeyBox  GroupKeyManager(AES-GCM) │
├────────────────────────────────────────────────────────┤
│  协议层 (protocol/)：定长头 + TLV 载荷，手写序列化         │
├────────────────────────────────────────────────────────┤
│  网络层 (net/)：MulticastSocket + 接收线程               │
└───────────────────────┬────────────────────────────────┘
                        ▼  UDP 多播 239.255.42.99:54321
```

**线程模型**：

- 主线程：Win32 消息循环，所有 UI 更新只发生在这里。
- 接收线程：阻塞 `recvfrom` → 解析校验 → `PostMessage(WM_APP_RX)` 把堆上报文递给主线程处理（UI 免锁）。
- 文件发送线程：按 pacing 节拍分块发送（`Sleep` 控速）。
- 主线程 `SetTimer`：每 3s 发心跳、每秒检查成员超时、驱动重传轮次。

## 4. 网络参数

| 参数 | 值 | 说明 |
|---|---|---|
| 组地址:端口 | 239.255.42.99:54321 | 239/8 本地组织范围，端口避开特权段 |
| TTL | 1（可配置） | 同网段；跨一个路由器时设 2 |
| `IP_MULTICAST_LOOP` | ON | **单机开多实例演示的关键** |
| `SO_REUSEADDR` | ON | 多实例绑定同一端口 |
| `SO_RCVBUF` | 4 MB | 吸收文件分块的突发，减少内核层丢包 |
| 报文上限 | ≤ 1400 B | 以太网 MTU 1500 内，避免 IP 分片放大丢包 |

## 5. 报文协议（自设计，二进制）

### 5.1 定长头（32 字节，网络字节序）

```
偏移  大小  字段         说明
0     4    magic        'MCHT'
4     1    version      1
5     1    type         消息类型（见 5.2）
6     2    flags        bit0=载荷已加密 bit1=带 RSA 签名
8     16   senderId     UUIDv4（成员唯一标识）
24    4    seq          发送端单调递增序号（排序/防重放）
28    4    payloadLen   载荷长度
32    ..   payload
```

序列化不用 struct 直接 cast（有 padding/对齐陷阱），统一走手写
`put_u16/u32/u64/utf8/bytes` 的 codec，跨机稳定。

### 5.2 消息类型

| type | 名称 | 载荷关键字段 | 用途 |
|---|---|---|---|
| 0x01 | HELLO | name, joinTime, RSA 公钥 blob | 入组宣告 |
| 0x02 | HELLO_ACK | 同上 + targetId | 老成员应答新成员 |
| 0x03 | HEARTBEAT | — | 3s 一次保活 |
| 0x04 | BYE | reason | 正常退组 |
| 0x05 | KICK | targetId + 房主 RSA 签名 | 踢人 |
| 0x06 | KEY_DIST | {memberId: RSA-OAEP(组密钥)} | 组密钥信封分发 |
| 0x10 | TEXT | text（加密后），签名 | 聊天文本 |
| 0x20 | FILE_META | fileId, name, size, sha256, chunkSize, totalChunks | 文件开始 |
| 0x21 | FILE_DATA | fileId, chunkSeq, data(≤1024B) | 文件分块 |
| 0x22 | FILE_END | fileId | 首轮发完 |
| 0x23 | NACK | fileId, missingSeqs[] | 请求重传 |
| 0x24 | FILE_CANCEL | fileId, reason | 中途取消 |

## 6. 成员管理设计（需求 c）

组播的组成员关系在 IP 层（IGMP），应用**查不到**，所以"显示/删除成员"必须在
应用层自建成员表——这正是本设计要回答的问题：

1. **建表**：新成员启动后多播 `HELLO`；在线成员收到后记表并回 `HELLO_ACK`（带自己
   的 name/公钥/joinTime），新成员据此建全表，UI 刷新成员列表（名字、IP、状态、
   房主标记、公钥指纹）。
2. **保活与删除**：每 3s `HEARTBEAT`；10s 无心跳标记离线（列表置灰），20s 移除；
   收到 `BYE` 立即移除；退出程序钩子保证发 BYE。
3. **房主**：joinTime 最早且在线者担任（房间创建者自然为第一任），房主离线自动
   继任。踢人仅房主可发起：`KICK` 携带房主 RSA 私钥签名，全员可验签；被踢端收到
   验签通过后自动 `leaveGroup` 退出并弹提示。
4. **强制离组**（进阶）：踢人后房主重新生成组密钥并 `KEY_DIST`，被踢者不在分发
   名单里，拿不到新密钥 → 即便赖在组里也解不开后续内容。

**M2 实现备注**（已完成）：

- 心跳/HELLO 载荷自带昵称 + joinTime：任何报文都能修复成员表（HELLO 丢包后心跳自动补全）。
- HELLO_ACK/心跳学习到的成员静默入表（不刷“加入”消息），只有 HELLO 触发加入通知。
- 房主选举用系统时钟毫秒（跨机可比），同毫秒按 UUID 决胜；房主变更在聊天区提示。
- 踢人 = 多播 `KICK(targetId)` + 本地黑名单；所有成员校验发送方等于当前房主
  （RSA 验签在 M4 叠加）。
- 被踢端收到 KICK 后禁用输入，3 秒后自动退出。
- UI：成员列表右键“踢出该成员”（仅房主可用）；状态栏实时显示成员数与本机角色。

## 7. 文件可靠传输设计（需求 b）

UDP 多播不可靠，大文件必须自己补可靠性（本设计 = 简化 NORM）：

- **分块**：1024 B/块，fileId=UUID，chunkSeq 从 0 编号。
- **节奏发送**：发送线程定时发送，默认 250 块/秒（≈250 KB/s，UI 可调）；
  突发灌包是丢包首因。
- **接收状态机**：META 建立接收上下文（位图 + tmp 文件）→ 收块置位写盘 →
  END 后对比位图得缺失集 → 多播 `NACK`（缺失 seq 列表）→ 发送端只重传缺失块
  → 最多 5 轮仍未收全则报失败。
- **完整性**：META 带 SHA-256（CNG 计算），收全后校验，不符报错。
- **播放**：成功后移入 `received/<原名>`，UI 出现"▶ 播放"按钮，
  `ShellExecute` 交给系统默认播放器；传输中显示进度条。
- 多个接收方各自独立收，发送方一次多播、无需逐个发送（1-N 语义天然满足）。

## 8. 安全设计（需求 d，加分项）

密钥分层：**RSA-2048 管身份和"发钥匙"，AES-256-GCM 管实际加密**（非对称算法
不适合直接加密大文件，信封方案才是正确姿势——报告里是加分论点）。

```
① 每个成员启动时用 CNG 生成 RSA-2048 密钥对，
   公钥 blob 随 HELLO 多播广播。
② 房主生成 32B 组会话密钥，构造 KEY_DIST：
   对每个在线成员 → RSA-OAEP(SHA-256) 加密同一把 AES 密钥，
   打包成 {memberId: 密文} 一条多播发出（信封分发）。
③ 各成员用自己 RSA 私钥解出组密钥。
④ 此后 TEXT / FILE_* 载荷全部 AES-256-GCM 加密
   （nonce 96bit 由 senderId+seq 派生，seq 单调防重放）。
⑤ 成员变动/踢人 → 房主重新生成密钥再分发（被踢者被排除在外）。
```

- UI 提供"加密"开关，方便 Wireshark 抓包对比演示（报告好素材）。
- 可选：TEXT 附 RSA-PSS 签名（flags bit1），防伪造发送者。

## 9. 目录结构

```
mcast-chat/
├── ARCHITECTURE.md          本文档
├── README.md                编译与使用说明
├── build.bat / Makefile     一键编译（g++，链接 ws2_32/comctl32/rpcrt4 等）
├── demo.bat                 一键启动 3 实例演示（ZhangSan/LiSi/WangWu）
├── app.manifest / app.rc    嵌入清单：启用 comctl32 v6 现代控件样式
├── test/
│   ├── nettest.cpp/.exe     网络层自测：同进程双 socket 验证同机多播分发
│   └── smoke.ps1            双实例端到端冒烟测试（配合隐藏参数 --send）
└── src/
    ├── main.cpp             入口：命令行/启动顺序/消息循环          [M1 ✔]
    ├── app.hpp/.cpp         全局 App：昵称/UUID/发送序号/句柄       [M1 ✔]
    ├── net/
    │   ├── multicast.hpp/.cpp   socket 创建/加组/TTL/回环/收发      [M1 ✔]
    │   └── rx_thread.hpp/.cpp   接收线程 → PostMessage 投递 UI      [M1 ✔]
    ├── protocol/
    │   ├── message.hpp      消息类型枚举、32B 头、常量              [M1 ✔]
    │   └── codec.hpp/.cpp   手写大端序列化 + 编包/解包              [M1 ✔]
    ├── core/
    │   ├── chat_ctrl.hpp/.cpp   聊天组包与发送                      [M1 ✔]
    │   ├── member_mgr.hpp/.cpp  成员表/握手/心跳超时/房主/踢人      [M2 ✔]
    │   └── file_xfer.hpp/.cpp   文件收发状态机 + pacing + NACK      [M3 待做]
    ├── crypto/              （M4 待做）CNG RSA-2048 / AES-256-GCM / 组密钥分发
    ├── util/
    │   ├── text.hpp/.cpp    UTF-8 ↔ UTF-16 转换、UTF-8 安全截断     [M1 ✔]
    │   └── log.hpp/.cpp     debug.log 调试日志                      [M1 ✔]
    └── ui/
        └── main_window.hpp/.cpp  主窗口：聊天区/成员列表/输入/状态栏 [M1 ✔]
```

## 10. 构建与演示

```bat
:: build.bat
g++ -std=c++20 -O2 -Wall -o mcast-chat.exe src\*.cpp src\net\*.cpp src\protocol\*.cpp src\core\*.cpp src\crypto\*.cpp src\ui\*.cpp -lws2_32 -lbcrypt -lshell32 -liphlpapi
```

- **单机演示 1-N**：开 3 个实例（LOOP=ON + SO_REUSEADDR），互发文字、传 mp4。
- **真局域网**：同一台路由器/交换机下多台电脑。注意**校园/机房 WiFi 常屏蔽多播**，
  演示首选手机热点或有线路由。
- **抓包**：Wireshark 过滤 `udp.port == 54321`，开/关加密各截一张图。
- 首次运行放行 Windows 防火墙（入站 UDP 54321）。

## 11. 风险与对策

| 风险 | 对策 |
|---|---|
| WiFi/AP 屏蔽多播 | 提前测试；热点/有线兜底；单机多实例保底 |
| 多播丢包 | NACK 重传 + 位图；pacing 可调低；SO_RCVBUF 调大 |
| 报文分片放大丢包 | 单报文 ≤1400B |
| 被踢者仍留在组 | 换组密钥，使其无法解密（应用层强制） |
| MinGW 字符集 | 内部统一 UTF-8，与 Win32 宽字符 API 交界用 MultiByteToWideChar 转换 |
| 程序异常退出残留成员 | 心跳超时自动清除 |

## 12. 开发里程碑

| 阶段 | 内容 | 验收 |
|---|---|---|
| M1 | 骨架：窗口 + 多播收发 + TEXT 收发 | 多实例互聊 |
| M2 | 成员管理：握手建表/心跳/列表/踢人 | 列表实时、踢人生效 |
| M3 | 文件传输：分块/NACK/校验/播放 | 传 mp4 收全可播 |
| M4 | 安全：RSA+AES 信封、加密开关 | 抓包只见密文 |
| M5 | 报告/演示脚本/边界打磨 | 答辩 |
