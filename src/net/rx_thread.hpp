#pragma once
namespace net {

bool start_rx_thread();
void stop_rx_thread();  // 置退出标志并等待线程收尾（≤2s）

}  // namespace net
