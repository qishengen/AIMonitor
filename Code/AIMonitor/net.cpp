#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>
#include "net.h"
#include "transport.h"

static net_cfg_t g_cfg;
static WiFiServer g_server(NET_DEFAULT_PORT);
static WiFiClient g_clients[NET_MAX_CLIENTS];
static bool g_server_on = false;
static char g_ip_str[16] = "0.0.0.0";
static uint32_t g_sta_since = 0;  // 最近一次启用 STA 的时刻（纯 STA 兜底计时）
static uint32_t g_gen = 0;        // net_apply 次数（web 检测重绑用）

// ---------------- 小工具 ----------------

static void trim(char* s) {
  size_t n = strlen(s);
  while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\r' || s[n-1] == '\n' || s[n-1] == '\t')) s[--n] = 0;
  char* p = s;
  while (*p == ' ' || *p == '\t') p++;
  if (p != s) memmove(s, p, strlen(p) + 1);
}

static int parse_ip(const char* s, uint8_t* out) {
  unsigned a[4];
  if (sscanf(s, "%u.%u.%u.%u", &a[0], &a[1], &a[2], &a[3]) != 4) return 0;
  for (int i = 0; i < 4; i++) if (a[i] > 255) return 0;
  for (int i = 0; i < 4; i++) out[i] = (uint8_t)a[i];
  return 1;
}

static bool mode_has_ap(const net_cfg_t* c) {
  return strcmp(c->mode, "ap") == 0 || strcmp(c->mode, "ap+sta") == 0;
}

static bool mode_has_sta(const net_cfg_t* c) {
  return strcmp(c->mode, "sta") == 0 || strcmp(c->mode, "ap+sta") == 0;
}

// ---------------- 配置持久化 ----------------

void net_default_cfg(net_cfg_t* c) {
  memset(c, 0, sizeof(*c));
  strcpy(c->mode, "ap+sta");  // AP 默认常开（热点始终可达，STA 有 ssid 才连）
  c->port = NET_DEFAULT_PORT;
  c->have_static = false;
}

void net_load_cfg(net_cfg_t* c) {
  net_default_cfg(c);
  File f = LittleFS.open(NET_CFG_PATH, "r");
  if (!f) return;
  char line[80];
  int n;
  while ((n = f.readBytesUntil('\n', line, sizeof(line) - 1)) > 0) {
    line[n] = 0;
    trim(line);
    char* eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    char* key = line;
    char* val = eq + 1;
    if (strcmp(key, "mode") == 0) {
      strncpy(c->mode, val, sizeof(c->mode) - 1);
      c->mode[sizeof(c->mode) - 1] = 0;
    } else if (strcmp(key, "ssid") == 0) {
      strncpy(c->ssid, val, sizeof(c->ssid) - 1);
      c->ssid[sizeof(c->ssid) - 1] = 0;
    } else if (strcmp(key, "pass") == 0) {
      strncpy(c->pass, val, sizeof(c->pass) - 1);
      c->pass[sizeof(c->pass) - 1] = 0;
    } else if (strcmp(key, "ap_ssid") == 0) {
      strncpy(c->ap_ssid, val, sizeof(c->ap_ssid) - 1);
      c->ap_ssid[sizeof(c->ap_ssid) - 1] = 0;
    } else if (strcmp(key, "ap_pass") == 0) {
      strncpy(c->ap_pass, val, sizeof(c->ap_pass) - 1);
      c->ap_pass[sizeof(c->ap_pass) - 1] = 0;
    } else if (strcmp(key, "port") == 0) {
      c->port = (uint16_t)atoi(val);
    } else if (strcmp(key, "ip") == 0) {
      if (parse_ip(val, c->ip)) c->have_static = true;
    } else if (strcmp(key, "gw") == 0) {
      parse_ip(val, c->gw);
    } else if (strcmp(key, "mask") == 0) {
      parse_ip(val, c->mask);
    }
  }
  f.close();
}

void net_save_cfg(const net_cfg_t* c) {
  File f = LittleFS.open(NET_CFG_PATH, "w");
  if (!f) return;
  f.printf("mode=%s\n", c->mode);
  f.printf("ssid=%s\n", c->ssid);
  f.printf("pass=%s\n", c->pass);
  if (c->ap_ssid[0]) f.printf("ap_ssid=%s\n", c->ap_ssid);
  if (c->ap_pass[0]) f.printf("ap_pass=%s\n", c->ap_pass);
  f.printf("port=%u\n", (unsigned)c->port);
  if (c->have_static) {
    f.printf("ip=%u.%u.%u.%u\ngw=%u.%u.%u.%u\nmask=%u.%u.%u.%u\n",
             c->ip[0], c->ip[1], c->ip[2], c->ip[3],
             c->gw[0], c->gw[1], c->gw[2], c->gw[3],
             c->mask[0], c->mask[1], c->mask[2], c->mask[3]);
  }
  f.close();
}

void net_factory_reset(void) {
  LittleFS.remove(NET_CFG_PATH);
}

// ---------------- WiFi + TCP 启动 ----------------

void net_apply(const net_cfg_t* c) {
  g_cfg = *c;
  g_gen++;
  if (g_server_on) { g_server.close(); g_server_on = false; }
  for (int i = 0; i < NET_MAX_CLIENTS; i++) g_clients[i].stop();

  WiFi.persistent(false);
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);

  if (mode_has_ap(&g_cfg) && mode_has_sta(&g_cfg)) WiFi.mode(WIFI_AP_STA);
  else if (mode_has_ap(&g_cfg)) WiFi.mode(WIFI_AP);
  else WiFi.mode(WIFI_STA);

  if (mode_has_ap(&g_cfg)) {
    char ssid[40];
    if (g_cfg.ap_ssid[0]) {
      strncpy(ssid, g_cfg.ap_ssid, sizeof(ssid) - 1);
      ssid[sizeof(ssid) - 1] = 0;
    } else {
      snprintf(ssid, sizeof(ssid), "AIMonitor-%06X", (unsigned)ESP.getChipId());
    }
    // AP 热点密码独立（空=开放，≥8 位启用 WPA2）；max_connection=8 提高客户端上限
    const char* appass = g_cfg.ap_pass[0] ? g_cfg.ap_pass : NULL;
    WiFi.softAP(ssid, appass, 1, 0, 8);
  }
  if (mode_has_sta(&g_cfg)) {
    if (g_cfg.have_static) {
      IPAddress ip(g_cfg.ip[0], g_cfg.ip[1], g_cfg.ip[2], g_cfg.ip[3]);
      IPAddress gw(g_cfg.gw[0], g_cfg.gw[1], g_cfg.gw[2], g_cfg.gw[3]);
      IPAddress mask(g_cfg.mask[0], g_cfg.mask[1], g_cfg.mask[2], g_cfg.mask[3]);
      WiFi.config(ip, gw, mask);
    }
    if (g_cfg.ssid[0]) WiFi.begin(g_cfg.ssid, g_cfg.pass);
  }

  g_server.close();
  g_server.begin(g_cfg.port);
  g_server_on = true;

  if (mode_has_sta(&g_cfg)) {
    g_sta_since = millis();
    MDNS.begin("aimonitor");  // 局域网内可用 aimonitor.local 访问
  } else {
    g_sta_since = 0;
  }
}

void net_setup(void) {
  net_load_cfg(&g_cfg);
  net_apply(&g_cfg);
}

// ---------------- 主循环轮询 ----------------

void net_poll(void) {
  // 纯 STA 模式连接超时兜底：保留配网门户（ap+sta），不持久化，STA 成功连上则不触发
  if (g_sta_since && mode_has_sta(&g_cfg) && !mode_has_ap(&g_cfg) &&
      WiFi.status() != WL_CONNECTED && (millis() - g_sta_since) > NET_STA_FALLBACK_MS) {
    strcpy(g_cfg.mode, "ap+sta");
    net_apply(&g_cfg);
  }

  MDNS.update();

  if (g_server_on) {
    WiFiClient nc = g_server.accept();
    if (nc) {
      for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (!g_clients[i]) { g_clients[i] = nc; break; }
      }
      // 无空闲槽位则丢弃（nc 析构自动关闭）
    }
  }
  for (int i = 0; i < NET_MAX_CLIENTS; i++) {
    if (!g_clients[i]) continue;
    if (!g_clients[i].connected()) { g_clients[i].stop(); continue; }
    int c;
    while ((c = g_clients[i].read()) >= 0) {
      transport_feed((src_id_t)(SRC_TCP0 + i), (uint8_t)c);
    }
  }
}

void net_broadcast(const uint8_t* data, size_t len) {
  for (int i = 0; i < NET_MAX_CLIENTS; i++) {
    if (g_clients[i]) g_clients[i].write(data, len);
  }
}

bool net_client_write(int idx, const uint8_t* data, size_t len) {
  if (idx < 0 || idx >= NET_MAX_CLIENTS) return false;
  if (!g_clients[idx]) return false;
  return g_clients[idx].write(data, len) == len;
}

int net_client_read(int idx) {
  if (idx < 0 || idx >= NET_MAX_CLIENTS) return -1;
  if (!g_clients[idx]) return -1;
  return g_clients[idx].read();
}

uint32_t net_generation(void) { return g_gen; }

// ---------------- 状态查询 ----------------

const char* net_mode_str(void) { return g_cfg.mode; }
const char* net_ssid_str(void) { return g_cfg.ssid; }
const char* net_pass_str(void) { return g_cfg.pass; }
const char* net_ap_pass_str(void) { return g_cfg.ap_pass; }
uint16_t net_port(void) { return g_cfg.port; }

const char* net_ap_ssid_str(void) {
  static char s[40];
  if (g_cfg.ap_ssid[0]) strncpy(s, g_cfg.ap_ssid, sizeof(s) - 1);
  else snprintf(s, sizeof(s), "AIMonitor-%06X", (unsigned)ESP.getChipId());
  s[sizeof(s) - 1] = 0;
  return s;
}

int net_client_count(void) {
  int n = 0;
  for (int i = 0; i < NET_MAX_CLIENTS; i++) if (g_clients[i]) n++;
  return n;
}

bool net_sta_connected(void) { return WiFi.status() == WL_CONNECTED; }
bool net_has_sta(void) { return mode_has_sta(&g_cfg); }
bool net_has_ap(void) { return mode_has_ap(&g_cfg); }

int net_rssi(void) {
  return net_sta_connected() ? (int)WiFi.RSSI() : 0;
}

const char* net_ip_str(void) {
  IPAddress ip;
  if (mode_has_sta(&g_cfg) && WiFi.status() == WL_CONNECTED) ip = WiFi.localIP();
  else ip = WiFi.softAPIP();
  if (ip == IPAddress(0, 0, 0, 0)) ip = WiFi.softAPIP();
  snprintf(g_ip_str, sizeof(g_ip_str), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  return g_ip_str;
}

const char* net_ap_ip_str(void) {
  static char s[16];
  IPAddress ip = WiFi.softAPIP();
  snprintf(s, sizeof(s), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  return s;
}

const char* net_sta_ip_str(void) {
  static char s[16];
  if (mode_has_sta(&g_cfg) && WiFi.status() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    snprintf(s, sizeof(s), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  } else {
    snprintf(s, sizeof(s), "0.0.0.0");
  }
  return s;
}
