#pragma once
#include <stdint.h>
#include <stdbool.h>

#define ANM_FRAME_BYTES 1024u

bool fs_init(void);
uint32_t fs_total_bytes(void);
uint32_t fs_free_bytes(void);

// ---- upload：分块写入 .anm（断点续传） ----
bool fs_upload_begin(const char* name, uint32_t size);  // 同名同尺寸保留(续传)，否则重建
uint32_t fs_upload_offset(void);                        // 当前磁盘上已写字节数
bool fs_upload_seek(uint32_t off);                      // 打开活动文件并定位
bool fs_upload_append(const uint8_t* data, uint32_t len); // 顺序追加
void fs_upload_close(void);
bool fs_upload_verify_active(uint32_t crc);             // 整文件 CRC32 校验，失败删除

// ---- transmit：整动画一次传入（payload=完整 .anm，含 8B 头），仅保存 ----
bool fs_transmit_begin(const char* name);
bool fs_transmit_append(const uint8_t* data, uint32_t len);
void fs_transmit_close(void);

// ---- play：按帧读取 ----
bool fs_anim_open(const char* name);
uint8_t fs_anim_fps(void);
uint16_t fs_anim_count(void);
bool fs_anim_read_frame(uint16_t seq, uint8_t* buf);

// ---- 管理 ----
bool fs_delete(const char* name);
bool fs_list(char* out, int outsz);
bool fs_list_array(char* out, int outsz);  // 仅输出 .anm 文件名数组 ["a.anm",...]
