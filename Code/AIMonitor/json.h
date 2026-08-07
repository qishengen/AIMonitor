#pragma once
#include <stdint.h>

// 最小单层 JSON 解析：只支持 { "key":value, ... }，value 为字符串或整数。
// 用于解析上位机命令头。未知字段忽略。
bool json_get_string(const char* json, const char* key, char* out, int outsz);
bool json_get_int(const char* json, const char* key, long long* out);
bool json_has(const char* json, const char* key);

// base64 流式解码（4字符 -> 最多3字节）。每包调用 init 一次。
typedef struct {
  uint8_t buf[4];
  uint8_t n;
} b64dec_t;

void b64dec_init(b64dec_t* d);
// 返回写入 out 的字节数(0..3)；返回 -1 表示该字符为填充/非法(应跳过，继续)。
int b64dec_put(b64dec_t* d, char c, uint8_t* out);
// 收尾：返回剩余解码字节数(0..2)，置空内部状态。
int b64dec_finish(b64dec_t* d, uint8_t* out);
