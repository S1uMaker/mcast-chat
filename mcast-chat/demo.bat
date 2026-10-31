@echo off
rem One-click demo: 3 instances on one machine (multicast loopback + SO_REUSEADDR)
rem Pass any nickname as an argument to mcast-chat.exe, e.g. mcast-chat.exe ZhangSan
start "" mcast-chat.exe ZhangSan
ping -n 2 127.0.0.1 >nul
start "" mcast-chat.exe LiSi
ping -n 2 127.0.0.1 >nul
start "" mcast-chat.exe WangWu
echo 3 instances started: ZhangSan / LiSi / WangWu
