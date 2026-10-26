/*
 * util.c — engine text helpers: control stripping, sentence trimming.
 */

#include "asngn_internal.h"

#include <string.h>

/* ---- control stripping -------------------------------------------------- */

size_t asngn_strip_controls(char *s, size_t len) {
  size_t r, w = 0;
  if (!s) return 0;
  for (r = 0; r < len; r++) {
    unsigned char ch = (unsigned char)s[r];
    if ((ch < 0x20 && ch != '\n' && ch != '\t') || ch == 0x7F)
      continue; /* NUL and other controls stripped, DEL too */
    if (w != r) s[w] = s[r];
    w++;
  }
  if (w < len) s[w] = '\0';
  return w;
}

/* ---- sentence trimming -------------------------------------------------- */

static bool is_ascii_space(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' ||
         ch == '\f';
}

/* Last i < len with s[i] in ".!?" followed by whitespace/EOL, or a '\n';
 * the later boundary wins (backward scan returns it first). Boundaries are
 * ASCII, so a UTF-8 sequence is never split. */
size_t asngn_sentence_trim(const char *s, size_t len) {
  size_t i;
  if (!s || len == 0) return len;
  for (i = len; i-- > 0;) {
    char ch = s[i];
    if (ch == '\n') return i + 1;
    if ((ch == '.' || ch == '!' || ch == '?') &&
        (i + 1 == len || is_ascii_space(s[i + 1])))
      return i + 1;
  }
  return len;
}
