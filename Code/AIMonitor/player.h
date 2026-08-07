#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef void (*frame_cb)(const uint8_t* frame);                      // 需要上屏时回调
typedef void (*event_cb)(const char* type, uint32_t seq, int fps, uint32_t extra); // "progress"/"done"

void player_init(frame_cb render, event_cb ev);

// 协议接收帧数据时的目标缓冲（当前背面缓冲），之后调用 show_now / stream_submit。
uint8_t* player_recv_buf(void);
void player_show_now(const uint8_t* src);   // show：立即上屏
void player_stream_submit(const uint8_t* src); // stream：交给播放器，按 fps 定时上屏

bool player_play(const char* name, int fps, bool loop);
void player_stop(void);
bool player_stream_start(int fps, bool loop, int total);
void player_stream_end(void);
void player_tick(void); // 非阻塞，需在主循环高频调用

const char* player_state_str(void);
const char* player_anim_name(void);
uint32_t player_seq(void);
int player_fps(void);
