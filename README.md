# mcast-chat

带有文件传输功能的多播聊天工具（网络编程课程大作业）。基于 **Winsock2 UDP 多播 +
Win32 API + Windows CNG**，零第三方依赖，无中心服务器。设计详见
[ARCHITECTURE.md](ARCHITECTURE.md)。

## 编译

```bat
build.bat
```

或 `mingw32-make`（需要 MinGW-w64 g++，本机 `C:\mingw64` 已验证）。

## 运行

```bat
mcast-chat.exe [昵称]
```

一键演示（单机开 3 个实例，多播回环 + 端口复用实现 1-N）：

```bat
demo.bat
```

- 聊天：输入框输文字，回车或点"发送"。
- 状态栏显示多播组、昵称和收到的消息条数。
- 首次运行如弹出 Windows 防火墙提示，请放行（入站 UDP 54321）。

> 提示：多播要求同一二层网络。校园/机房 WiFi 常屏蔽多播，跨机器演示请用手机
> 热点或有线路由；单机多实例演示不受影响。

## 测试

```bat
:: 网络层自测（同进程双 socket，验证同机多播分发与回环）
test\nettest.exe

:: 双实例端到端冒烟测试（本地回显 + 跨实例收发，输出 PASS/FAIL）
powershell -NoProfile -ExecutionPolicy Bypass -File test\smoke.ps1
```

隐藏参数 `--send <文本>`：启动后延时 1.2s 自动发送一条文本，供自动化测试使用。
排查问题时可看工作目录下的 `debug.log`。

## 当前进度

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M1 | 窗口 + 多播收发 + 文本互聊 | ✔ 完成（冒烟测试 PASS） |
| M2 | 成员管理：握手建表/心跳/列表/踢人 | 待做 |
| M3 | 文件传输：分块/NACK/SHA-256/播放 | 待做 |
| M4 | 安全：RSA+AES 组密钥信封分发、加密开关 | 待做 |
| M5 | 报告/演示脚本 | 待做 |
