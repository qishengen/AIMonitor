#pragma once
#include <stdint.h>
#include <stdbool.h>

#define NET_MAX_CLIENTS 2
#define NET_DEFAULT_PORT 8088
#define NET_CFG_PATH "/wifi.cfg"
#define NET_STA_FALLBACK_MS 15000UL  // 纯 STA 模式连不上时，多久后自动退回 ap+sta 保留配网门户

typedef struct {
  char mode[8];          // "ap" | "sta" | "ap+sta"
  char ssid[33];         // STA 要连接的路由器 SSID
  char pass[33];
  char ap_ssid[33];      // 热点名，为空则用 AIMonitor-<chipid>
  char ap_pass[33];      // AP 热点密码（空=开放热点；≥8 位启用 WPA2），独立于 STA pass
  uint16_t port;
  bool have_static;      // STA 静态 IP
  uint8_t ip[4], gw[4], mask[4];
} net_cfg_t;

void net_setup(void);           // 读配置并启动 WiFi + TCP server
void net_poll(void);            // 主循环高频调用：accept/读入/断线清理
void net_broadcast(const uint8_t* data, size_t len);  // 发给所有已连接 TCP 客户端
bool net_client_write(int idx, const uint8_t* data, size_t len);  // 发给单个客户端
int net_client_read(int idx);   // 非阻塞读一个字节，无数据返回 -1

void net_default_cfg(net_cfg_t* c);
void net_load_cfg(net_cfg_t* c);   // 从 /wifi.cfg 读取
void net_save_cfg(const net_cfg_t* c);
void net_apply(const net_cfg_t* c); // 应用并重启 WiFi/TCP（当前连接会被断开）
void net_factory_reset(void);       // 删除 /wifi.cfg（恢复出厂）

uint32_t net_generation(void);      // 每次 net_apply 自增（供 web 检测并重绑 server）

const char* net_ip_str(void);       // 当前生效 IP（STA 已连则 STA IP，否则 AP IP）
const char* net_ap_ip_str(void);    // 热点 IP
const char* net_sta_ip_str(void);   // STA IP（未连接为 0.0.0.0）
const char* net_ap_ssid_str(void);  // 热点名
const char* net_mode_str(void);
const char* net_ssid_str(void);
const char* net_pass_str(void);
const char* net_ap_pass_str(void);
uint16_t net_port(void);
int net_client_count(void);
int net_rssi(void);
bool net_sta_connected(void);
bool net_has_sta(void);
bool net_has_ap(void);
