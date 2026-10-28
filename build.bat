@echo off
setlocal
set CXXFLAGS=-std=c++20 -O2 -Wall -Wextra -DWIN32_LEAN_AND_MEAN -municode -mwindows -static -static-libgcc -static-libstdc++
set SRCS=src\main.cpp src\app.cpp src\util\text.cpp src\util\log.cpp src\net\multicast.cpp src\net\rx_thread.cpp src\protocol\codec.cpp src\core\chat_ctrl.cpp src\ui\main_window.cpp
set LIBS=-lws2_32 -lcomctl32 -lshell32 -lrpcrt4 -lgdi32 -luser32

windres app.rc -O coff -o app_res.o
if errorlevel 1 (
  echo windres FAILED
  exit /b 1
)

g++ %CXXFLAGS% -o mcast-chat.exe app_res.o %SRCS% %LIBS%
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
echo BUILD OK: mcast-chat.exe
