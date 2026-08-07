#include "json.h"
#include <string.h>

// 返回指向 key 值起始位置的指针（字符串值为开引号后，整数为数字起始），失败返回 NULL。
static const char* find_value(const char* s, const char* key) {
  int klen = (int)strlen(key);
  const char* p = s;
  while ((p = strchr(p, '"')) != NULL) {
    if (strncmp(p + 1, key, klen) == 0 && p[klen + 1] == '"') {
      if (p != s && !(p[-1] == '{' || p[-1] == ',' || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n')) {
        p++;
        continue;
      }
      const char* q = p + klen + 2;  // 跳过关键字闭引号
      while (*q == ' ' || *q == '\t') q++;
      if (*q == ':') {
        q++;
        while (*q == ' ' || *q == '\t') q++;
        return q;
      }
    }
    p++;
  }
  return NULL;
}

bool json_get_string(const char* s, const char* key, char* out, int outsz) {
  if (outsz <= 0) return false;
  const char* v = find_value(s, key);
  if (!v || *v != '"') return false;
  v++;
  int n = 0;
  while (*v && *v != '"' && n < outsz - 1) {
    if (*v == '\\') {
      v++;
      if (!*v) break;
    }
    out[n++] = *v++;
  }
  out[n] = 0;
  return true;
}

bool json_get_int(const char* s, const char* key, long long* out) {
  const char* v = find_value(s, key);
  if (!v) return false;
  bool neg = false;
  if (*v == '-') { neg = true; v++; }
  if (*v < '0' || *v > '9') return false;
  long long val = 0;
  while (*v >= '0' && *v <= '9') {
    val = val * 10 + (*v - '0');
    v++;
  }
  *out = neg ? -val : val;
  return true;
}

bool json_has(const char* s, const char* key) {
  return find_value(s, key) != NULL;
}

// ---------------- base64 ----------------

static int b64_val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static int b64_group(const uint8_t* b, int n, uint8_t* out) {
  uint32_t x = ((uint32_t)b[0] << 18) | ((uint32_t)b[1] << 12);
  if (n >= 3) x |= ((uint32_t)b[2] << 6);
  if (n >= 4) x |= (uint32_t)b[3];
  int cnt = 0;
  out[cnt++] = (uint8_t)(x >> 16);
  if (n >= 3) out[cnt++] = (uint8_t)(x >> 8);
  if (n >= 4) out[cnt++] = (uint8_t)x;
  return cnt;
}

void b64dec_init(b64dec_t* d) { d->n = 0; }

int b64dec_put(b64dec_t* d, char c, uint8_t* out) {
  int v = b64_val(c);
  if (v < 0) return -1;  // '=' 或非法字符
  d->buf[d->n++] = (uint8_t)v;
  if (d->n == 4) {
    int cnt = b64_group(d->buf, 4, out);
    d->n = 0;
    return cnt;
  }
  return 0;
}

int b64dec_finish(b64dec_t* d, uint8_t* out) {
  if (d->n == 0) return 0;
  int cnt = b64_group(d->buf, d->n, out);
  d->n = 0;
  return cnt;
}
