#pragma once
#include <stdint.h>

// 协议层（数据处理层）：只做命令语义，不碰字节。
// 字节收发/帧组装由通信层 transport 负责；本层注册回调，通过 transport_send_line /
// transport_broadcast_line 输出回包（应答回源、异步事件广播）。

void protocol_setup(void);
void protocol_tick(void);        // 主循环延迟任务（如延迟重启）
void on_player_event(const char* type, uint32_t seq, int fps, uint32_t extra);

// Web 命令同步执行：独立于帧状态机（不经 transport 帧组装），应答捕获到 out。
// 返回捕获字节数；<=0 表示无应答。line 无需以 '\n' 结尾。
int protocol_web_exec(const char* line, int len, char* out, int outsz);
