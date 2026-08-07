#pragma once
#include <stdint.h>

void protocol_setup(void);
void protocol_poll(void);           // 主循环高频调用：收串口 → 协议状态机 → 分发命令

// player 事件回调（progress/done），实现在 protocol.cpp，上报到串口。
void on_player_event(const char* type, uint32_t seq, int fps, uint32_t extra);
