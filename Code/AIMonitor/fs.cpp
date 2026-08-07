#include <Arduino.h>
#include <LittleFS.h>
#include <string.h>
#include "fs.h"

static File g_file;          // 活动上传/传输文件句柄
static char g_up_name[64] = "";  // 活动上传/传输文件名
static char g_anim_name[64] = ""; // 当前播放动画名
static uint8_t g_anim_fps = 0;
static uint16_t g_anim_count = 0;

// ---------------- CRC32 (zlib 兼容) ----------------

static uint32_t crc_tab[256];
static bool crc_tab_ok = false;

static void crc32_build_tab(void) {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    crc_tab[i] = c;
  }
  crc_tab_ok = true;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t* d, uint32_t len) {
  if (!crc_tab_ok) crc32_build_tab();
  for (uint32_t i = 0; i < len; i++) crc = crc_tab[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}

static uint32_t crc32_calc(const uint8_t* d, uint32_t len) { return ~crc32_update(~0u, d, len); }

static uint32_t crc32_file(const char* name) {
  File f = LittleFS.open(name, "r");
  if (!f) return 0;
  uint32_t crc = ~0u;
  uint8_t buf[64];
  int r;
  while ((r = f.read(buf, sizeof(buf))) > 0) crc = crc32_update(crc, buf, (uint32_t)r);
  f.close();
  return ~crc;
}

// ---------------- 基础 ----------------

bool fs_init(void) {
  if (LittleFS.begin()) return true;
  // 首次烧录后 LittleFS 尚未格式化：自动格式化并重挂载
  if (LittleFS.format()) return LittleFS.begin();
  return false;
}

uint32_t fs_total_bytes(void) {
  FSInfo i;
  LittleFS.info(i);
  return i.totalBytes;
}

uint32_t fs_free_bytes(void) {
  FSInfo i;
  LittleFS.info(i);
  return (i.totalBytes > i.usedBytes) ? (i.totalBytes - i.usedBytes) : 0;
}

// ---------------- upload ----------------

bool fs_upload_begin(const char* name, uint32_t size) {
  strncpy(g_up_name, name, sizeof(g_up_name) - 1);
  g_up_name[sizeof(g_up_name) - 1] = 0;
  fs_upload_close();
  if (LittleFS.exists(name)) {
    File f = LittleFS.open(name, "r");
    uint32_t sz = f.size();
    f.close();
    if (sz != size) LittleFS.remove(name);
  }
  File f = LittleFS.open(name, "a+");
  if (!f) return false;
  f.close();
  return true;
}

uint32_t fs_upload_offset(void) {
  if (!g_up_name[0]) return 0;
  File f = LittleFS.open(g_up_name, "r");
  if (!f) return 0;
  uint32_t s = f.size();
  f.close();
  return s;
}

bool fs_upload_seek(uint32_t off) {
  fs_upload_close();
  if (!g_up_name[0]) return false;
  File f = LittleFS.open(g_up_name, "r+");
  if (!f) {
    f = LittleFS.open(g_up_name, "a+");
    if (!f) return false;
  }
  if (!f.seek(off)) {
    f.close();
    return false;
  }
  g_file = f;
  return true;
}

bool fs_upload_append(const uint8_t* data, uint32_t len) {
  if (!g_file) return false;
  return g_file.write(data, len) == len;
}

void fs_upload_close(void) {
  if (g_file) g_file.close();
}

bool fs_upload_verify_active(uint32_t crc) {
  if (!g_up_name[0]) return false;
  uint32_t c = crc32_file(g_up_name);
  if (c == crc) return true;
  LittleFS.remove(g_up_name);
  return false;
}

// ---------------- transmit ----------------
// 整动画一次传入：payload 即完整 .anm（含 8B 头），原样落盘，不自动补头。

bool fs_transmit_begin(const char* name) {
  strncpy(g_up_name, name, sizeof(g_up_name) - 1);
  g_up_name[sizeof(g_up_name) - 1] = 0;
  fs_transmit_close();
  LittleFS.remove(name);
  File f = LittleFS.open(name, "w");
  if (!f) return false;
  g_file = f;
  return true;
}

bool fs_transmit_append(const uint8_t* data, uint32_t len) {
  if (!g_file) return false;
  return g_file.write(data, len) == len;
}

void fs_transmit_close(void) {
  if (g_file) g_file.close();
}

// ---------------- play ----------------

bool fs_anim_open(const char* name) {
  File f = LittleFS.open(name, "r");
  if (!f) return false;
  uint32_t size = f.size();
  if (size < 8) { f.close(); return false; }
  uint8_t hdr[8];
  if (f.read(hdr, sizeof(hdr)) != sizeof(hdr) ||
      hdr[0] != 'A' || hdr[1] != 'N' || hdr[2] != 'M' || hdr[3] != '0') {
    f.close();
    return false;
  }
  uint16_t count = (uint16_t)(hdr[5] | ((uint16_t)hdr[6] << 8));
  if (size != 8 + (uint32_t)count * ANM_FRAME_BYTES) {
    f.close();
    return false;
  }
  f.close();
  g_anim_fps = hdr[4];
  g_anim_count = count;
  strncpy(g_anim_name, name, sizeof(g_anim_name) - 1);
  g_anim_name[sizeof(g_anim_name) - 1] = 0;
  return true;
}

uint8_t fs_anim_fps(void) { return g_anim_fps; }
uint16_t fs_anim_count(void) { return g_anim_count; }

bool fs_anim_read_frame(uint16_t seq, uint8_t* buf) {
  if (!g_anim_name[0]) return false;
  File f = LittleFS.open(g_anim_name, "r");
  if (!f) return false;
  if (!f.seek(8 + (uint32_t)seq * ANM_FRAME_BYTES)) { f.close(); return false; }
  int r = f.read(buf, ANM_FRAME_BYTES);
  f.close();
  return r == (int)ANM_FRAME_BYTES;
}

// ---------------- 管理 ----------------

bool fs_delete(const char* name) {
  if (!LittleFS.exists(name)) return false;
  return LittleFS.remove(name);
}

bool fs_list(char* out, int outsz) {
  if (outsz <= 0) return false;
  int pos = snprintf(out, outsz, "{\"resp\":\"list\",\"anims\":[");
  if (pos < 0 || pos >= outsz) return false;
  Dir dir = LittleFS.openDir("/");
  bool first = true;
  while (dir.next()) {
    String n = dir.fileName();
    if (!n.endsWith(".anm")) continue;
    if (pos >= outsz - 3) break;
    if (!first) out[pos++] = ',';
    first = false;
    int w = snprintf(out + pos, outsz - pos, "\"%s\"", n.c_str());
    if (w < 0) return false;
    pos += w;
    if (pos > outsz - 3) pos = outsz - 3;
  }
  out[pos++] = ']';
  out[pos++] = '}';
  out[pos] = 0;
  return true;
}
