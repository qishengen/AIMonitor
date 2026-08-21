#include <Arduino.h>
#include <string.h>
#include "transport.h"
#include "net.h"
#include "json.h"

#define RING_SIZE 8192
#define LINE_SIZE 256
#define BATCH 256
#define PACKET_TIMEOUT_MS 1000   // 收包不完整超时(数据丢字节时复位，避免永久卡死)

static const char B64_MARK[] = "\"data\":\"";
#define B64_MARK_LEN (sizeof(B64_MARK) - 1)

enum { RX_LINE, RX_B64, RX_TAIL, RX_DATA };

static tp_line_cb g_on_line = NULL;
static tp_header_cb g_on_header = NULL;
static tp_payload_cb g_on_payload = NULL;
static tp_done_cb g_on_done = NULL;
static tp_timeout_cb g_on_timeout = NULL;

static uint8_t ring[RING_SIZE];
static uint16_t rhead = 0, rtail = 0;
static uint32_t rx_last_activity = 0;

static char line[LINE_SIZE];
static int llen = 0;
static bool line_overflow = false;

static int b64mark = 0;
static b64dec_t b64ctx;
static uint8_t batch[BATCH];
static int batch_n = 0;

static int rx_state = RX_LINE;
static src_id_t g_src = SRC_SERIAL;   // 当前处理字节的来源（应答/超时路由）

static bool pkt_active = false;   // 数据包进行中（等待数据体）
static uint32_t pkt_total = 0;
static uint32_t pkt_got = 0;

// Web 同步执行输出捕获
static uint8_t* g_exec_out = NULL;
static size_t g_exec_outsz = 0;
static size_t g_exec_n = 0;

// ---------------- 输出路由 ----------------

static void send_to_src(src_id_t src, const uint8_t* d, size_t n) {
  switch (src) {
    case SRC_SERIAL:
      Serial.write(d, n);
      break;
    case SRC_TCP0:
      net_client_write(0, d, n);
      break;
    case SRC_TCP1:
      net_client_write(1, d, n);
      break;
    case SRC_WEB:
      if (g_exec_out && g_exec_n + n <= g_exec_outsz) {
        memcpy(g_exec_out + g_exec_n, d, n);
        g_exec_n += n;
      }
      break;
    default:
      break;
  }
}

void transport_send_line(src_id_t src, const char* s) {
  size_t n = strlen(s);
  send_to_src(src, (const uint8_t*)s, n);
  send_to_src(src, (const uint8_t*)"\n", 1);
}

void transport_broadcast_line(const char* s) {
  Serial.print(s);
  Serial.print('\n');
  net_broadcast((const uint8_t*)s, strlen(s));
  net_broadcast((const uint8_t*)"\n", 1);
}

// ---------------- 帧状态机 ----------------

static void reset_packet(void) {
  llen = 0;
  line[0] = 0;
  line_overflow = false;
  b64mark = 0;
  batch_n = 0;
  pkt_active = false;
  pkt_total = 0;
  pkt_got = 0;
  rx_state = RX_LINE;
}

void transport_reset(void) { reset_packet(); }

// 冲刷当前批；写入失败时协议层已回 nack，本层复位。
static bool flush_batch(void) {
  if (batch_n == 0) return true;
  bool ok = g_on_payload && g_on_payload(g_src, batch, (uint32_t)batch_n);
  batch_n = 0;
  if (!ok) reset_packet();
  return ok;
}

static void on_b64_marker(void) {
  line[llen] = 0;
  int size = g_on_header ? g_on_header(g_src, line, llen, true) : TP_CMD_REJECT;
  if (size >= 0) {
    pkt_active = true;
    pkt_total = (uint32_t)size;
    pkt_got = 0;
    b64dec_init(&b64ctx);
    batch_n = 0;
    rx_state = RX_B64;
  } else {
    rx_state = RX_TAIL;  // 拒绝/异常：跳到行尾
  }
  llen = 0;
  line[0] = 0;
}

static void on_line_complete(void) {
  if (llen == 0) return;
  line[llen] = 0;
  int size = g_on_header ? g_on_header(g_src, line, llen, false) : TP_CMD_LINE;
  if (size >= 0) {
    pkt_active = true;
    pkt_total = (uint32_t)size;
    pkt_got = 0;
    rx_state = RX_DATA;
  } else if (size == TP_CMD_LINE && g_on_line) {
    g_on_line(g_src, line, llen);
  }
  // TP_CMD_REJECT：已 nack，丢弃本行
}

static void update_marker(char c) {
  if (b64mark < 0) return;
  while (b64mark > 0 && c != B64_MARK[b64mark]) b64mark = 0;
  if (c == B64_MARK[b64mark]) b64mark++;
  if (b64mark == B64_MARK_LEN) {
    b64mark = -1;
    on_b64_marker();
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
          pkt_got += (uint32_t)n;
          for (int i = 0; i < n; i++) {
            batch[batch_n++] = dec[i];
            if (batch_n >= BATCH && !flush_batch()) return;
          }
        }
        rx_state = RX_TAIL;
        return;
      }
      uint8_t dec[3];
      int r = b64dec_put(&b64ctx, (char)b, dec);
      if (r > 0) {
        pkt_got += (uint32_t)r;
        for (int i = 0; i < r; i++) {
          batch[batch_n++] = dec[i];
          if (batch_n >= BATCH && !flush_batch()) return;
        }
        if (pkt_got >= pkt_total) rx_state = RX_TAIL;
      }
      return;
    }
    case RX_TAIL: {
      if (b == '\n') {
        if (pkt_active) {
          if (!flush_batch()) return;
          if (g_on_done) g_on_done(g_src);
        }
        reset_packet();
      }
      return;
    }
    case RX_DATA: {
      batch[batch_n++] = b;
      pkt_got++;
      if (batch_n >= BATCH && !flush_batch()) return;
      if (pkt_got >= pkt_total) {
        if (!flush_batch()) return;
        if (g_on_done) g_on_done(g_src);
        reset_packet();
      }
      return;
    }
    default:
      return;
  }
}

// ---------------- 入口 ----------------

void transport_init(tp_line_cb on_line, tp_header_cb on_header,
                    tp_payload_cb on_payload, tp_done_cb on_done,
                    tp_timeout_cb on_timeout) {
  g_on_line = on_line;
  g_on_header = on_header;
  g_on_payload = on_payload;
  g_on_done = on_done;
  g_on_timeout = on_timeout;
  reset_packet();
}

static bool ring_full(void) { return ((rtail + 1) % RING_SIZE) == rhead; }

void transport_feed(src_id_t src, uint8_t b) {
  if (ring_full()) return;  // ring 满则丢弃
  g_src = src;
  ring[rtail] = b;
  rtail = (uint16_t)((rtail + 1) % RING_SIZE);
  rx_last_activity = millis();
}

void transport_poll(void) {
  while ((int)Serial.available() > 0) transport_feed(SRC_SERIAL, (uint8_t)Serial.read());
  while (rhead != rtail) {
    uint8_t b = ring[rhead];
    rhead = (uint16_t)((rhead + 1) % RING_SIZE);
    rx_last_activity = millis();
    process_byte(b);
  }
  // 收包超时保护：数据不完整超时后复位并回 nack，避免永久卡死
  if (rx_state != RX_LINE && millis() - rx_last_activity > PACKET_TIMEOUT_MS) {
    src_id_t src = g_src;
    reset_packet();
    if (g_on_timeout) g_on_timeout(src);
  }
}

// Web 同步执行：把命令字节(通常带 '\n')一次性注入管线，捕获回包到 out。
int transport_exec(const uint8_t* bytes, size_t len, uint8_t* out, size_t outsz) {
  if (rx_state != RX_LINE) return -1;  // 有数据包进行中，拒绝
  g_src = SRC_WEB;
  g_exec_out = out;
  g_exec_outsz = outsz;
  g_exec_n = 0;
  for (size_t i = 0; i < len; i++) process_byte(bytes[i]);
  int n = (int)g_exec_n;
  g_exec_out = NULL;
  g_exec_outsz = 0;
  g_exec_n = 0;
  if (rx_state != RX_LINE) reset_packet();  // 防御：Web 不应进入数据模式
  return n;
}

// Web 回包捕获：协议层直接执行命令（不经帧状态机）时，SRC_WEB 应答写入 out。
void transport_capture_begin(uint8_t* out, size_t outsz) {
  g_exec_out = out;
  g_exec_outsz = outsz;
  g_exec_n = 0;
}

int transport_capture_end(void) {
  int n = (int)g_exec_n;
  g_exec_out = NULL;
  g_exec_outsz = 0;
  g_exec_n = 0;
  return n;
}
