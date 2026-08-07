#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include "json.h"
#include "fs.h"
#include "player.h"
#include "protocol.h"

#define FW_VERSION "1.1.0"
#define PROTO_VERSION 1

#define RING_SIZE 8192
#define LINE_SIZE 256
#define BATCH 256
#define PACKET_TIMEOUT_MS 1000   // 收包不完整超时(数据丢字节时复位，避免永久卡死)

static const char B64_MARK[] = "\"data\":\"";
#define B64_MARK_LEN (sizeof(B64_MARK) - 1)

enum { RX_LINE, RX_B64, RX_TAIL, RX_DATA };

static uint8_t ring[RING_SIZE];
static uint16_t rhead = 0, rtail = 0;
static uint32_t rx_last_activity = 0;

static char line[LINE_SIZE];
static int llen = 0;
static bool line_overflow = false;

static int b64mark = 0;
static b64dec_t b64ctx;
static uint8_t b64batch[BATCH];
static int b64batch_n = 0;

static int rx_state = RX_LINE;

static int pending_cmd = -1;
static const uint8_t* pending_dst = NULL;
static uint32_t pending_total = 0;
static uint32_t pending_got = 0;

enum {
  CMD_PING, CMD_STATUS, CMD_STOP, CMD_LIST, CMD_DELETE, CMD_PLAY,
  CMD_SHOW, CMD_STREAM_START, CMD_FRAME, CMD_STREAM_END,
  CMD_UP_BEGIN, CMD_UP_CHUNK, CMD_UP_END, CMD_TRANSMIT,
  CMD_NONE = -1
};

// ---------------- 发送 ----------------

static void send(const char* s) {
  Serial.print(s);
  Serial.print('\n');
}

static void send_ack(void) { send("{\"resp\":\"ack\"}"); }

static bool send_nack(int code) {
  char b[48];
  snprintf(b, sizeof(b), "{\"resp\":\"nack\",\"code\":%d}", code);
  send(b);
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
  switch (pending_cmd) {
    case CMD_SHOW:
    case CMD_FRAME:
      if (pending_dst && pending_got + n <= pending_total) {
        memcpy((uint8_t*)pending_dst + pending_got, d, n);
        return true;
      }
      return false;
    case CMD_UP_CHUNK:
      return fs_upload_append(d, n);
    case CMD_TRANSMIT:
      return fs_transmit_append(d, n);
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
      pending_dst = player_recv_buf();
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
  pending_cmd = id;
  pending_total = (uint32_t)size;
  pending_got = 0;
  return true;
}

static void flush_b64(void) {
  if (b64batch_n == 0) return;
  sink_write(b64batch, (uint32_t)b64batch_n);
  b64batch_n = 0;
}

static void cmd_finish_data(int id) {
  switch (id) {
    case CMD_SHOW:
      player_show_now(pending_dst);
      send_ack();
      break;
    case CMD_FRAME:
      player_stream_submit(pending_dst);
      send_ack();
      break;
    case CMD_UP_CHUNK:
      fs_upload_close();
      send_ack();
      break;
    case CMD_TRANSMIT:
      fs_transmit_close();
      send("{\"resp\":\"done\"}");
      break;
    default:
      send_nack(1);
  }
}

static void finalize_data(void) {
  if (pending_got != pending_total) {
    if (pending_cmd == CMD_UP_CHUNK) fs_upload_close();
    if (pending_cmd == CMD_TRANSMIT) fs_transmit_close();
    send_nack(2);
    return;
  }
  cmd_finish_data(pending_cmd);
}

static void reset_packet(void) {
  llen = 0;
  line[0] = 0;
  line_overflow = false;
  b64mark = 0;
  b64batch_n = 0;
  pending_cmd = -1;
  pending_dst = NULL;
  pending_total = 0;
  pending_got = 0;
  rx_state = RX_LINE;
}

// ---------------- 命令分发 ----------------

static void dispatch_cmd(int id, const char* json) {
  char name[64];
  long long v;
  switch (id) {
    case CMD_PING: {
      char b[128];
      snprintf(b, sizeof(b),
               "{\"resp\":\"pong\",\"fw\":\"%s\",\"proto\":%d,\"fs_total\":%lu,\"fs_free\":%lu}",
               FW_VERSION, PROTO_VERSION,
               (unsigned long)fs_total_bytes(), (unsigned long)fs_free_bytes());
      send(b);
      break;
    }
    case CMD_STATUS: {
      char b[128];
      snprintf(b, sizeof(b), "{\"resp\":\"state\",\"state\":\"%s\",\"name\":\"%s\",\"seq\":%lu}",
               player_state_str(), player_anim_name(), (unsigned long)player_seq());
      send(b);
      break;
    }
    case CMD_STOP:
      player_stop();
      send("{\"resp\":\"state\",\"state\":\"idle\",\"name\":\"\",\"seq\":0}");
      break;
    case CMD_LIST: {
      char b[512];
      if (fs_list(b, sizeof(b))) send(b);
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
        send(b);
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
      send("{\"resp\":\"state\",\"state\":\"stream\",\"name\":\"\",\"seq\":0}");
      break;
    }
    case CMD_STREAM_END:
      player_stream_end();
      send("{\"resp\":\"done\"}");
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
      send(b);
      break;
    }
    case CMD_UP_END: {
      long long crc = 0;
      if (!json_get_int(json, "crc", &crc)) { send_nack(2); break; }
      if (!fs_upload_verify_active((uint32_t)crc)) { send_nack(4); break; }
      send("{\"resp\":\"done\"}");
      break;
    }
    default:
      send_nack(1);
  }
}

// ---------------- 协议状态机 ----------------

static void on_line_complete(void) {
  if (llen == 0) return;
  line[llen] = 0;
  char cmd[24];
  if (!json_get_string(line, "cmd", cmd, sizeof(cmd))) { send_nack(1); return; }
  int id = cmd_index(cmd);
  if (id == CMD_NONE) { send_nack(1); return; }
  if (is_data_cmd(id)) {
    if (cmd_prepare_data(id, line)) rx_state = RX_DATA;
  } else {
    dispatch_cmd(id, line);
  }
}

static void on_b64_marker(void) {
  line[llen] = 0;
  char cmd[24];
  if (!json_get_string(line, "cmd", cmd, sizeof(cmd))) { send_nack(1); rx_state = RX_TAIL; return; }
  int id = cmd_index(cmd);
  if (id == CMD_NONE || !is_data_cmd(id)) { send_nack(1); rx_state = RX_TAIL; return; }
  if (!cmd_prepare_data(id, line)) { rx_state = RX_TAIL; return; }  // nack 已发
  b64dec_init(&b64ctx);
  b64batch_n = 0;
  rx_state = RX_B64;
}

static void update_marker(char c) {
  if (b64mark < 0) return;
  while (b64mark > 0 && c != B64_MARK[b64mark]) b64mark = 0;
  if (c == B64_MARK[b64mark]) b64mark++;
  if (b64mark == B64_MARK_LEN) {
    b64mark = -1;
    on_b64_marker();
    llen = 0;
    line[0] = 0;
  }
}

static void process_byte(uint8_t b) {
  switch (rx_state) {
    case RX_LINE: {
      if (b == '\n') {
        on_line_complete();
        llen = 0;
        b64mark = 0;
        line_overflow = false;
        return;
      }
      if (line_overflow) return;
      if (llen < LINE_SIZE - 1) {
        line[llen++] = (char)b;
        line[llen] = 0;
        update_marker((char)b);
      } else {
        line_overflow = true;
      }
      return;
    }
    case RX_B64: {
      if (b == '"') {
        uint8_t dec[3];
        int n = b64dec_finish(&b64ctx, dec);
        if (n > 0) {
          pending_got += (uint32_t)n;
          for (int i = 0; i < n; i++) {
            b64batch[b64batch_n++] = dec[i];
            if (b64batch_n >= BATCH) flush_b64();
          }
        }
        rx_state = RX_TAIL;
        return;
      }
      uint8_t dec[3];
      int r = b64dec_put(&b64ctx, (char)b, dec);
      if (r > 0) {
        pending_got += (uint32_t)r;
        for (int i = 0; i < r; i++) {
          b64batch[b64batch_n++] = dec[i];
          if (b64batch_n >= BATCH) flush_b64();
        }
        if (pending_got >= pending_total) rx_state = RX_TAIL;
      }
      return;
    }
    case RX_TAIL: {
      if (b == '\n') {
        if (pending_cmd >= 0) {
          flush_b64();
          finalize_data();
        }
        reset_packet();
      }
      return;
    }
    default:
      return;
  }
}

void protocol_setup(void) {
  rhead = 0;
  rtail = 0;
  rx_last_activity = 0;
  reset_packet();
}

static bool ring_full(void) { return ((rtail + 1) % RING_SIZE) == rhead; }

void protocol_poll(void) {
  while ((int)Serial.available() > 0 && !ring_full()) {
    ring[rtail] = (uint8_t)Serial.read();
    rtail = (uint16_t)((rtail + 1) % RING_SIZE);
    rx_last_activity = millis();
  }
  while (rhead != rtail) {
    if (rx_state == RX_DATA) {
      uint8_t batch[BATCH];
      uint32_t n = 0;
      while (n < BATCH && rhead != rtail && pending_got < pending_total) {
        batch[n++] = ring[rhead];
        rhead = (uint16_t)((rhead + 1) % RING_SIZE);
        pending_got++;
      }
      rx_last_activity = millis();
      if (n > 0 && !sink_write(batch, n)) {
        if (pending_cmd == CMD_UP_CHUNK) fs_upload_close();
        if (pending_cmd == CMD_TRANSMIT) fs_transmit_close();
        send_nack(2);
        reset_packet();
        continue;
      }
      if (pending_got >= pending_total) {
        cmd_finish_data(pending_cmd);
        reset_packet();
      }
    } else {
      uint8_t b = ring[rhead];
      rhead = (uint16_t)((rhead + 1) % RING_SIZE);
      rx_last_activity = millis();
      process_byte(b);
    }
  }
  // 收包超时保护：数据不完整超时后复位并回 nack，避免永久卡死
  if (rx_state != RX_LINE && millis() - rx_last_activity > PACKET_TIMEOUT_MS) {
    reset_packet();
    send_nack(2);
  }
}

// ---------------- player 事件上报 ----------------

void on_player_event(const char* type, uint32_t seq, int fps, uint32_t extra) {
  (void)extra;
  if (strcmp(type, "progress") == 0) {
    char b[80];
    snprintf(b, sizeof(b), "{\"resp\":\"progress\",\"seq\":%lu,\"fps\":%d}",
             (unsigned long)seq, fps);
    send(b);
  } else if (strcmp(type, "done") == 0) {
    send("{\"resp\":\"done\"}");
  }
}
