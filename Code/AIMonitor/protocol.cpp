#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include "json.h"
#include "fs.h"
#include "player.h"
#include "protocol.h"
#include "net.h"
#include "transport.h"
#include "version.h"

enum {
  CMD_PING, CMD_STATUS, CMD_STOP, CMD_LIST, CMD_DELETE, CMD_PLAY,
  CMD_SHOW, CMD_STREAM_START, CMD_FRAME, CMD_STREAM_END,
  CMD_UP_BEGIN, CMD_UP_CHUNK, CMD_UP_END, CMD_TRANSMIT,
  CMD_WIFI, CMD_WIFI_STATUS, CMD_AP_PASS,
  CMD_RESET, CMD_FACTORY, CMD_HOME, CMD_WIFI_RECONNECT,
  CMD_NONE = -1
};

static src_id_t g_cmd_src = SRC_SERIAL;  // 当前命令来源（应答路由）
static int g_pending_cmd = -1;
static const uint8_t* g_pending_dst = NULL;
static uint32_t g_pending_total = 0;
static uint32_t g_pending_got = 0;

static uint32_t g_restart_at = 0;   // 延迟重启时刻（0=不重启）
static uint32_t g_reapply_at = 0;   // 延迟应用配置时刻（0=不应用）

// ---------------- 发送（应答回源） ----------------

static void send_src(const char* s) { transport_send_line(g_cmd_src, s); }
static void send_ack(void) { send_src("{\"resp\":\"ack\"}"); }

static bool send_nack(int code) {
  char b[48];
  snprintf(b, sizeof(b), "{\"resp\":\"nack\",\"code\":%d}", code);
  send_src(b);
  return false;
}

// ---------------- 命令索引 ----------------

static int cmd_index(const char* cmd) {
  static const struct { const char* n; int id; } tab[] = {
    {"ping", CMD_PING}, {"status", CMD_STATUS}, {"stop", CMD_STOP},
    {"list", CMD_LIST}, {"delete", CMD_DELETE}, {"play", CMD_PLAY},
    {"show", CMD_SHOW}, {"stream_start", CMD_STREAM_START}, {"frame", CMD_FRAME},
    {"stream_end", CMD_STREAM_END}, {"up_begin", CMD_UP_BEGIN},
    {"up_chunk", CMD_UP_CHUNK}, {"up_end", CMD_UP_END}, {"transmit", CMD_TRANSMIT},
    {"wifi", CMD_WIFI}, {"wifi_status", CMD_WIFI_STATUS}, {"ap_pass", CMD_AP_PASS},
    {"reset", CMD_RESET}, {"factory", CMD_FACTORY},
    {"home", CMD_HOME}, {"wifi_reconnect", CMD_WIFI_RECONNECT},
  };
  for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
    if (strcmp(cmd, tab[i].n) == 0) return tab[i].id;
  return CMD_NONE;
}

static bool is_data_cmd(int id) {
  return id == CMD_SHOW || id == CMD_FRAME || id == CMD_UP_CHUNK || id == CMD_TRANSMIT;
}

// ---------------- 数据接收 ----------------

// 数据通道写入目标：show/frame → 播放器背面缓冲；up_chunk/transmit → 文件
static bool sink_write(const uint8_t* d, uint32_t n) {
  switch (g_pending_cmd) {
    case CMD_SHOW:
    case CMD_FRAME:
      if (g_pending_dst && g_pending_got + n <= g_pending_total) {
        memcpy((uint8_t*)g_pending_dst + g_pending_got, d, n);
        g_pending_got += n;
        return true;
      }
      return false;
    case CMD_UP_CHUNK:
      if (fs_upload_append(d, n)) { g_pending_got += n; return true; }
      return false;
    case CMD_TRANSMIT:
      if (fs_transmit_append(d, n)) { g_pending_got += n; return true; }
      return false;
    default:
      return false;
  }
}

// 数据命令头已解析完毕：准备接收目标。返回 false 表示参数错（已发 nack）。
static bool cmd_prepare_data(int id, const char* json) {
  long long size = 0;
  if (!json_get_int(json, "size", &size) || size <= 0) return send_nack(2);
  switch (id) {
    case CMD_SHOW:
    case CMD_FRAME:
      if (size != (long long)ANM_FRAME_BYTES) return send_nack(2);
      g_pending_dst = player_recv_buf();
      break;
    case CMD_UP_CHUNK: {
      long long off = 0;
      if (!json_get_int(json, "off", &off) || off < 0) return send_nack(2);
      if (!fs_upload_seek((uint32_t)off)) return send_nack(2);  // 未 up_begin
      break;
    }
    case CMD_TRANSMIT: {
      if (size < 8 || (size - 8) % ANM_FRAME_BYTES != 0) return send_nack(2);
      char name[64];
      if (!json_get_string(json, "name", name, sizeof(name))) return send_nack(2);
      if (!fs_transmit_begin(name)) return send_nack(5);
      break;
    }
    default:
      return send_nack(1);
  }
  g_pending_cmd = id;
  g_pending_total = (uint32_t)size;
  g_pending_got = 0;
  return true;
}

static void cmd_finish_data(int id) {
  switch (id) {
    case CMD_SHOW:
      player_show_now(g_pending_dst);
      send_ack();
      break;
    case CMD_FRAME:
      player_stream_submit(g_pending_dst);
      send_ack();
      break;
    case CMD_UP_CHUNK:
      fs_upload_close();
      send_ack();
      break;
    case CMD_TRANSMIT:
      fs_transmit_close();
      send_src("{\"resp\":\"done\"}");
      break;
    default:
      send_nack(1);
  }
}

// ---------------- 命令分发 ----------------

static void dispatch_cmd(int id, const char* json) {
  char name[64];
  long long v;
  switch (id) {
    case CMD_PING: {
      char b[128];
      snprintf(b, sizeof(b),
               "{\"resp\":\"pong\",\"fw\":\"%s\",\"proto\":%d,\"ip\":\"%s\",\"fs_total\":%lu,\"fs_free\":%lu}",
               FW_VERSION, PROTO_VERSION, net_ip_str(),
               (unsigned long)fs_total_bytes(), (unsigned long)fs_free_bytes());
      send_src(b);
      break;
    }
    case CMD_STATUS: {
      char b[128];
      snprintf(b, sizeof(b), "{\"resp\":\"state\",\"state\":\"%s\",\"name\":\"%s\",\"seq\":%lu}",
               player_state_str(), player_anim_name(), (unsigned long)player_seq());
      send_src(b);
      break;
    }
    case CMD_STOP:
      player_stop();
      send_src("{\"resp\":\"state\",\"state\":\"idle\",\"name\":\"\",\"seq\":0}");
      break;
    case CMD_HOME:
      player_play_home();
      send_src("{\"resp\":\"state\",\"state\":\"home\",\"name\":\"\",\"seq\":0}");
      break;
    case CMD_LIST: {
      char b[512];
      if (fs_list(b, sizeof(b))) send_src(b);
      else send_nack(2);
      break;
    }
    case CMD_DELETE:
      if (json_get_string(json, "name", name, sizeof(name))) {
        if (fs_delete(name)) send_ack();
        else send_nack(3);
      } else send_nack(2);
      break;
    case CMD_PLAY: {
      if (!json_get_string(json, "name", name, sizeof(name))) { send_nack(2); break; }
      long long fps = 0, loop = 1;
      json_get_int(json, "fps", &fps);
      json_get_int(json, "loop", &loop);
      if (player_play(name, (int)fps, loop != 0)) {
        char b[128];
        snprintf(b, sizeof(b), "{\"resp\":\"state\",\"state\":\"play\",\"name\":\"%s\",\"seq\":0}", name);
        send_src(b);
      } else send_nack(3);
      break;
    }
    case CMD_STREAM_START: {
      long long fps = 24, loop = 0, total = 0;
      json_get_int(json, "fps", &fps);
      json_get_int(json, "loop", &loop);
      json_get_int(json, "total", &total);
      if (fps < 1) fps = 1;
      if (fps > 60) fps = 60;
      player_stream_start((int)fps, loop != 0, (int)total);
      send_src("{\"resp\":\"state\",\"state\":\"stream\",\"name\":\"\",\"seq\":0}");
      break;
    }
    case CMD_STREAM_END:
      player_stream_end();
      send_src("{\"resp\":\"done\"}");
      break;
    case CMD_UP_BEGIN: {
      if (!json_get_string(json, "name", name, sizeof(name))) { send_nack(2); break; }
      long long size = 0;
      if (!json_get_int(json, "size", &size) || size < 8 || (size - 8) % ANM_FRAME_BYTES != 0) {
        send_nack(2);
        break;
      }
      if (!fs_upload_begin(name, (uint32_t)size)) { send_nack(5); break; }
      char b[64];
      snprintf(b, sizeof(b), "{\"resp\":\"ready\",\"off\":%lu}", (unsigned long)fs_upload_offset());
      send_src(b);
      break;
    }
    case CMD_UP_END: {
      long long crc = 0;
      if (!json_get_int(json, "crc", &crc)) { send_nack(2); break; }
      if (!fs_upload_verify_active((uint32_t)crc)) { send_nack(4); break; }
      send_src("{\"resp\":\"done\"}");
      break;
    }
    case CMD_WIFI: {
      net_cfg_t c;
      net_load_cfg(&c);   // 以已存配置为基础，只覆盖下发字段
      char mode[16], ssid[33], pass[33];
      long long port = 0;
      bool has_mode = json_get_string(json, "mode", mode, sizeof(mode));
      bool has_ssid = json_get_string(json, "ssid", ssid, sizeof(ssid));
      bool has_pass = json_get_string(json, "pass", pass, sizeof(pass));
      bool has_port = json_get_int(json, "port", &port);
      if (!has_mode && !has_ssid && !has_pass && !has_port) { send_nack(2); break; }
      if (has_mode) {
        if (strcmp(mode, "ap") && strcmp(mode, "sta") && strcmp(mode, "ap+sta")) { send_nack(2); break; }
        strncpy(c.mode, mode, sizeof(c.mode) - 1);
        c.mode[sizeof(c.mode) - 1] = 0;
      }
      if (has_ssid) { strncpy(c.ssid, ssid, sizeof(c.ssid) - 1); c.ssid[sizeof(c.ssid) - 1] = 0; }
      if (has_pass) { strncpy(c.pass, pass, sizeof(c.pass) - 1); c.pass[sizeof(c.pass) - 1] = 0; }
      if (has_port) {
        if (port < 1 || port > 65535) { send_nack(2); break; }
        c.port = (uint16_t)port;
      }
      // 先回 ack 再延迟应用：net_apply 会断开连接，统一各通道（串口/TCP/Web）先收到回包
      send_ack();
      net_save_cfg(&c);
      g_reapply_at = millis() + 100;
      break;
    }
    case CMD_AP_PASS: {
      char ap[33];
      if (!json_get_string(json, "pass", ap, sizeof(ap))) { send_nack(2); break; }
      size_t alen = strlen(ap);
      if (alen > 0 && alen < 8) { send_nack(2); break; }  // WPA2 最短 8 位，空=开放
      net_cfg_t c;
      net_load_cfg(&c);
      strncpy(c.ap_pass, ap, sizeof(c.ap_pass) - 1);
      c.ap_pass[sizeof(c.ap_pass) - 1] = 0;
      send_ack();
      net_save_cfg(&c);
      g_reapply_at = millis() + 100;
      break;
    }
    case CMD_WIFI_RECONNECT: {
      net_cfg_t c;
      net_load_cfg(&c);
      send_ack();
      // 延迟应用：先让回包到达请求方（Web 场景下 net_apply 会断 WiFi，需在响应发出后执行）
      g_reapply_at = millis() + 100;
      break;
    }
    case CMD_WIFI_STATUS: {
      char b[160];
      snprintf(b, sizeof(b),
               "{\"resp\":\"wifi\",\"mode\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"port\":%u,\"clients\":%d,\"sta\":%d}",
               net_mode_str(), net_ssid_str(), net_ip_str(),
               (unsigned)net_port(), net_client_count(), net_sta_connected() ? 1 : 0);
      send_src(b);
      break;
    }
    case CMD_RESET:
      send_ack();
      g_restart_at = millis() + 300;  // 延迟重启，保证回包发出
      break;
    case CMD_FACTORY:
      net_factory_reset();
      send_ack();
      g_restart_at = millis() + 300;
      break;
    default:
      send_nack(1);
  }
}

// ---------------- transport 回调 ----------------

static void on_line(src_id_t src, const char* line, int len) {
  (void)len;
  g_cmd_src = src;
  char cmd[24];
  if (!json_get_string(line, "cmd", cmd, sizeof(cmd))) { send_nack(1); return; }
  int id = cmd_index(cmd);
  if (id == CMD_NONE) { send_nack(1); return; }
  dispatch_cmd(id, line);
}

static int on_header(src_id_t src, const char* line, int len, bool is_b64) {
  (void)len;
  g_cmd_src = src;
  char cmd[24];
  if (!json_get_string(line, "cmd", cmd, sizeof(cmd))) { send_nack(1); return TP_CMD_REJECT; }
  int id = cmd_index(cmd);
  if (id == CMD_NONE) { send_nack(1); return TP_CMD_REJECT; }
  if (!is_data_cmd(id)) {
    if (is_b64) { send_nack(1); return TP_CMD_REJECT; }  // 数据标记出现在非数据命令上
    return TP_CMD_LINE;                                  // 普通命令：由 on_line 分发
  }
  if (src == SRC_WEB) { send_nack(2); return TP_CMD_REJECT; }  // Web 通道不承载数据命令
  if (!cmd_prepare_data(id, line)) return TP_CMD_REJECT;  // nack 已发
  return (int)g_pending_total;
}

static bool on_payload(src_id_t src, const uint8_t* data, uint32_t len) {
  (void)src;
  if (!sink_write(data, len)) {
    if (g_pending_cmd == CMD_UP_CHUNK) fs_upload_close();
    if (g_pending_cmd == CMD_TRANSMIT) fs_transmit_close();
    send_nack(2);
    g_pending_cmd = -1;
    return false;
  }
  return true;
}

static void on_done(src_id_t src) {
  g_cmd_src = src;
  if (g_pending_cmd < 0) return;
  if (g_pending_got != g_pending_total) {
    if (g_pending_cmd == CMD_UP_CHUNK) fs_upload_close();
    if (g_pending_cmd == CMD_TRANSMIT) fs_transmit_close();
    send_nack(2);
    g_pending_cmd = -1;
    return;
  }
  cmd_finish_data(g_pending_cmd);
  g_pending_cmd = -1;
}

static void on_timeout(src_id_t src) {
  g_cmd_src = src;
  if (g_pending_cmd == CMD_UP_CHUNK) fs_upload_close();
  if (g_pending_cmd == CMD_TRANSMIT) fs_transmit_close();
  send_nack(2);
  g_pending_cmd = -1;
}

// ---------------- 初始化 / 主循环 ----------------

void protocol_setup(void) {
  transport_init(on_line, on_header, on_payload, on_done, on_timeout);
  g_pending_cmd = -1;
  g_restart_at = 0;
  g_reapply_at = 0;
}

void protocol_tick(void) {
  uint32_t now = millis();
  if (g_restart_at && (int32_t)(now - g_restart_at) >= 0) {
    ESP.restart();
  }
  if (g_reapply_at && (int32_t)(now - g_reapply_at) >= 0) {
    g_reapply_at = 0;
    net_cfg_t c;
    net_load_cfg(&c);
    net_apply(&c);
  }
}

// Web 命令同步执行：独立于帧状态机，直接解析并分发单行命令，应答捕获到 out。
// 不受串口/TCP 数据包占用影响；Web 不承载数据命令。
int protocol_web_exec(const char* line, int len, char* out, int outsz) {
  char buf[256];
  if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
  memcpy(buf, line, (size_t)len);
  buf[len] = 0;

  transport_capture_begin((uint8_t*)out, outsz);
  g_cmd_src = SRC_WEB;
  char cmd[24];
  int id = CMD_NONE;
  if (json_get_string(buf, "cmd", cmd, sizeof(cmd))) id = cmd_index(cmd);
  if (id == CMD_NONE) {
    send_nack(1);
  } else if (is_data_cmd(id)) {
    send_nack(2);  // Web 通道不承载数据命令
  } else {
    dispatch_cmd(id, buf);
  }
  return transport_capture_end();
}

// ---------------- player 事件上报（广播） ----------------

void on_player_event(const char* type, uint32_t seq, int fps, uint32_t extra) {
  (void)extra;
  if (strcmp(type, "progress") == 0) {
    char b[80];
    snprintf(b, sizeof(b), "{\"resp\":\"progress\",\"seq\":%lu,\"fps\":%d}",
             (unsigned long)seq, fps);
    transport_broadcast_line(b);
  } else if (strcmp(type, "done") == 0) {
    transport_broadcast_line("{\"resp\":\"done\"}");
  }
}
