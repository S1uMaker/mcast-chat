CXX      = g++
CXXFLAGS = -std=c++20 -O2 -Wall -Wextra -DWIN32_LEAN_AND_MEAN -municode -mwindows -static -static-libgcc -static-libstdc++
LIBS     = -lws2_32 -lcomctl32 -lcomdlg32 -lshell32 -lrpcrt4 -lgdi32 -luser32 -lbcrypt -lwinmm

SRCS = src/main.cpp src/app.cpp src/util/text.cpp src/util/log.cpp \
       src/net/multicast.cpp src/net/rx_thread.cpp \
       src/protocol/codec.cpp \
       src/core/chat_ctrl.cpp src/core/member_mgr.cpp src/core/file_xfer.cpp \
       src/crypto/sha256.cpp \
       src/ui/main_window.cpp

all: mcast-chat.exe

app_res.o: app.rc app.manifest
	windres app.rc -O coff -o app_res.o

mcast-chat.exe: $(SRCS) app_res.o
	$(CXX) $(CXXFLAGS) -o $@ app_res.o $(SRCS) $(LIBS)

run-demo: mcast-chat.exe
	cmd /c demo.bat

clean:
	cmd /c "del mcast-chat.exe app_res.o"

.PHONY: all run-demo clean
