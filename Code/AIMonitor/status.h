#pragma once
#include <stdint.h>

// 状态快照：聚合 net/fs/player/system 状态，供 Web /status 使用。
// 协议命令的回包（ping/status/wifi_status/list）仍由协议层各自构建。

uint32_t status_uptime(void);
int status_snapshot(char* out, int outsz);   // 返回写入字节数
