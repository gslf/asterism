/*
 * anchor.c — line anchors ("hashline") shared by fs.read, code.read-range,
 * grep.search and edit.lines.
 *
 * An anchor is "LINE:TAG": the 1-based line number and a 3-character tag of
 * the line's content. Tag rule: take the line's bytes without the line
 * terminator, drop trailing spaces, tabs and CRs, hash what is left with
 * 32-bit FNV-1a, xor the high half into the low half, and write the value
 * modulo 36^3 as three characters of [0-9a-z]. The tag depends on content
 * only, so a line keeps its tag when it moves; the line number is checked
 * separately. Anchored text shows each line as "LINE:TAG|text".
 */

#include "sdk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void astd_line_tag(const char *line, size_t len, char out[4]) {
  static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  uint32_t h = 2166136261u;
  size_t i;
  while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t' ||
                     line[len - 1] == '\r' || line[len - 1] == '\n'))
    len--;
  for (i = 0; i < len; i++) {
    h ^= (unsigned char)line[i];
    h *= 16777619u;
  }
  h = (h ^ (h >> 16)) % 46656u;
  out[0] = digits[h / 1296u];
  out[1] = digits[(h / 36u) % 36u];
  out[2] = digits[h % 36u];
  out[3] = '\0';
}

char *astd_anchor_text(const char *buf, size_t n, int64_t first_line,
                       size_t *out_n) {
  size_t lines = 0, i, start = 0, o = 0, cap;
  char *out;
  for (i = 0; i < n; i++)
    if (buf[i] == '\n') lines++;
  if (n > 0 && buf[n - 1] != '\n') lines++;
  /* per line: up to 20 digits, ':', 3 tag chars, '|' */
  if (lines > ((size_t)-1 - n - 1) / 25) return NULL;
  cap = n + lines * 25 + 1;
  out = malloc(cap);
  if (!out) return NULL;
  while (start < n) {
    const char *nl = memchr(buf + start, '\n', n - start);
    size_t end = nl ? (size_t)(nl - buf) + 1 : n;
    char tag[4];
    astd_line_tag(buf + start, end - start, tag);
    o += (size_t)snprintf(out + o, cap - o, "%lld:%s|", (long long)first_line,
                          tag);
    memcpy(out + o, buf + start, end - start);
    o += end - start;
    first_line++;
    start = end;
  }
  out[o] = '\0';
  if (out_n) *out_n = o;
  return out;
}

int astd_anchor_parse(const char *s, int64_t *line, char tag[4]) {
  int64_t v = 0;
  size_t i = 0, k;
  if (!s) return -1;
  while (s[i] == ' ') i++;
  if (s[i] < '0' || s[i] > '9') return -1;
  for (; s[i] >= '0' && s[i] <= '9'; i++) {
    if (v > 100000000000LL) return -1;
    v = v * 10 + (s[i] - '0');
  }
  if (v < 1 || s[i] != ':') return -1;
  i++;
  for (k = 0; k < 3; k++, i++) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'))) return -1;
    tag[k] = c;
  }
  tag[3] = '\0';
  /* a pasted "42:ab3|text" is accepted; the text is ignored */
  if (s[i] != '\0' && s[i] != '|' && s[i] != ' ') return -1;
  *line = v;
  return 0;
}
