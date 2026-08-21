#pragma once
#include <stdint.h>
#include <stdbool.h>

// 通信层（transport）：统一管理 串口 + TCP + Web 虚拟源 的字节收发与帧组装。
// 职责只到"字节/帧"为止，不理解命令语义；协议层通过回调收到完整命令并产出回包。
//
// 帧规则（与线上协议一致）：
//  - 普通命令：一行 JSON + '\n'
//  - 二进制数据命令：JSON 头 + '\n' + size 字节原始数据
//  - base64 数据命令：{"cmd":..,"size":N,"enc":"b64","data":"<b64>"} + '\n'
// transport 通过 on_header 返回值判断是否为数据命令，并自动组装数据体。

typedef enum {
  SRC_SERIAL = 0,   // 串口
  SRC_TCP0 = 1,     // TCP 客户端 0
  SRC_TCP1 = 2,     // TCP 客户端 1
  SRC_WEB = 3,      // Web 控制（同步执行）
  SRC_COUNT = 4
} src_id_t;

// on_header 返回值
#define TP_CMD_LINE   (-1)   // 普通命令：transport 随后回调 on_line
#define TP_CMD_REJECT (-2)   // 数据头但已拒绝（nack 已发），丢弃本行

// 回调（协议层注册）
typedef void (*tp_line_cb)(src_id_t src, const char* line, int len);
typedef int  (*tp_header_cb)(src_id_t src, const char* line, int len, bool is_b64); // >=0 = 数据体字节数
typedef bool (*tp_payload_cb)(src_id_t src, const uint8_t* data, uint32_t len);     // false = 写入失败
typedef void (*tp_done_cb)(src_id_t src);        // 数据体收齐（协议层收尾回包）
typedef void (*tp_timeout_cb)(src_id_t src);     // 收包超时（协议层回 nack）

void transport_init(tp_line_cb, tp_header_cb, tp_payload_cb, tp_done_cb, tp_timeout_cb);
void transport_poll(void);                              // 主循环：读串口 → 处理帧
void transport_feed(src_id_t src, uint8_t b);           // net 把 TCP 字节喂入
void transport_send_line(src_id_t src, const char* s);  // s + '\n' 回源
void transport_broadcast_line(const char* s);           // s + '\n' 广播（串口+所有TCP）
int  transport_exec(const uint8_t* bytes, size_t len, uint8_t* out, size_t outsz); // Web 同步执行
void transport_reset(void);                             // 复位帧状态
// Web 回包捕获（供协议层独立于帧状态机执行 Web 命令用）
void transport_capture_begin(uint8_t* out, size_t outsz);
int  transport_capture_end(void);   // 返回捕获字节数
