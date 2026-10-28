/*
 * crypto.c — ChaCha20, Poly1305 and AEAD_CHACHA20_POLY1305 (RFC 8439),
 * PBKDF2-HMAC-SHA256 (RFC 8018) over core's HMAC.
 *
 * Portable C99, no table lookups indexed by secret data and no
 * secret-dependent branches. Poly1305 follows the 26-bit-limb layout of
 * poly1305-donna (public domain). Every stack buffer that held key material
 * or keystream is zeroized before return.
 */

#include "asvault_internal.h"

#include <string.h>

/* ---- helpers ------------------------------------------------------------ */

void asvault_wipe(void *p, size_t n) {
  volatile uint8_t *b = (volatile uint8_t *)p;
  if (!p) return;
  while (n--) *b++ = 0;
}

static uint32_t ld32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void st32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static void st64(uint8_t *p, uint64_t v) {
  st32(p, (uint32_t)v);
  st32(p + 4, (uint32_t)(v >> 32));
}

#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

/* ---- ChaCha20 (RFC 8439 2.1-2.4) ---------------------------------------- */

void asvault_chacha20_quarter_round(uint32_t x[16], int a, int b, int c,
                                    int d) {
  x[a] += x[b]; x[d] ^= x[a]; x[d] = ROTL32(x[d], 16);
  x[c] += x[d]; x[b] ^= x[c]; x[b] = ROTL32(x[b], 12);
  x[a] += x[b]; x[d] ^= x[a]; x[d] = ROTL32(x[d], 8);
  x[c] += x[d]; x[b] ^= x[c]; x[b] = ROTL32(x[b], 7);
}

void asvault_chacha20_block(const uint8_t key[32], uint32_t counter,
                            const uint8_t nonce[12], uint8_t out[64]) {
  uint32_t s[16], x[16];
  int i;
  s[0] = 0x61707865u; /* "expand 32-byte k" */
  s[1] = 0x3320646eu;
  s[2] = 0x79622d32u;
  s[3] = 0x6b206574u;
  for (i = 0; i < 8; i++) s[4 + i] = ld32(key + 4 * i);
  s[12] = counter;
  s[13] = ld32(nonce);
  s[14] = ld32(nonce + 4);
  s[15] = ld32(nonce + 8);
  memcpy(x, s, sizeof x);
  for (i = 0; i < 10; i++) { /* 20 rounds: column then diagonal */
    asvault_chacha20_quarter_round(x, 0, 4, 8, 12);
    asvault_chacha20_quarter_round(x, 1, 5, 9, 13);
    asvault_chacha20_quarter_round(x, 2, 6, 10, 14);
    asvault_chacha20_quarter_round(x, 3, 7, 11, 15);
    asvault_chacha20_quarter_round(x, 0, 5, 10, 15);
    asvault_chacha20_quarter_round(x, 1, 6, 11, 12);
    asvault_chacha20_quarter_round(x, 2, 7, 8, 13);
    asvault_chacha20_quarter_round(x, 3, 4, 9, 14);
  }
  for (i = 0; i < 16; i++) st32(out + 4 * i, x[i] + s[i]);
  asvault_wipe(s, sizeof s);
  asvault_wipe(x, sizeof x);
}

void asvault_chacha20_xor(const uint8_t key[32], uint32_t counter,
                          const uint8_t nonce[12], const uint8_t *in,
                          uint8_t *out, size_t len) {
  uint8_t ks[64];
  size_t off = 0, i, n;
  while (off < len) {
    asvault_chacha20_block(key, counter++, nonce, ks);
    n = len - off < sizeof ks ? len - off : sizeof ks;
    for (i = 0; i < n; i++) out[off + i] = (uint8_t)(in[off + i] ^ ks[i]);
    off += n;
  }
  asvault_wipe(ks, sizeof ks);
}

/* ---- Poly1305 (RFC 8439 2.5) -------------------------------------------- */

void asvault_poly1305_init(asvault_poly1305 *st, const uint8_t key[32]) {
  /* r &= 0xffffffc0ffffffc0ffffffc0fffffff, split into 26-bit limbs */
  st->r[0] = (ld32(key + 0)) & 0x3ffffffu;
  st->r[1] = (ld32(key + 3) >> 2) & 0x3ffff03u;
  st->r[2] = (ld32(key + 6) >> 4) & 0x3ffc0ffu;
  st->r[3] = (ld32(key + 9) >> 6) & 0x3f03fffu;
  st->r[4] = (ld32(key + 12) >> 8) & 0x00fffffu;
  memset(st->h, 0, sizeof st->h);
  st->pad[0] = ld32(key + 16);
  st->pad[1] = ld32(key + 20);
  st->pad[2] = ld32(key + 24);
  st->pad[3] = ld32(key + 28);
  st->leftover = 0;
  st->final = 0;
}

static void poly_blocks(asvault_poly1305 *st, const uint8_t *m, size_t bytes) {
  const uint32_t hibit = st->final ? 0 : (1u << 24); /* 2^128 */
  uint32_t r0 = st->r[0], r1 = st->r[1], r2 = st->r[2], r3 = st->r[3],
           r4 = st->r[4];
  uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
  uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3],
           h4 = st->h[4];
  uint64_t d0, d1, d2, d3, d4;
  uint32_t c;

  while (bytes >= 16) {
    h0 += (ld32(m + 0)) & 0x3ffffffu;
    h1 += (ld32(m + 3) >> 2) & 0x3ffffffu;
    h2 += (ld32(m + 6) >> 4) & 0x3ffffffu;
    h3 += (ld32(m + 9) >> 6) & 0x3ffffffu;
    h4 += (ld32(m + 12) >> 8) | hibit;

    d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 +
         (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
    d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 +
         (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
    d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 +
         (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
    d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 +
         (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
    d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 +
         (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

    c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffffu;
    d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffffu;
    d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffffu;
    d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffffu;
    d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffffu;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffffu;
    h1 += c;

    m += 16;
    bytes -= 16;
  }
  st->h[0] = h0;
  st->h[1] = h1;
  st->h[2] = h2;
  st->h[3] = h3;
  st->h[4] = h4;
}

void asvault_poly1305_update(asvault_poly1305 *st, const uint8_t *m, size_t n) {
  size_t i, want;
  if (st->leftover) {
    want = 16 - st->leftover;
    if (want > n) want = n;
    for (i = 0; i < want; i++) st->buffer[st->leftover + i] = m[i];
    n -= want;
    m += want;
    st->leftover += want;
    if (st->leftover < 16) return;
    poly_blocks(st, st->buffer, 16);
    st->leftover = 0;
  }
  if (n >= 16) {
    want = n & ~(size_t)15;
    poly_blocks(st, m, want);
    m += want;
    n -= want;
  }
  for (i = 0; i < n; i++) st->buffer[st->leftover + i] = m[i];
  st->leftover += n;
}

void asvault_poly1305_final(asvault_poly1305 *st, uint8_t tag[16]) {
  uint32_t h0, h1, h2, h3, h4, c;
  uint32_t g0, g1, g2, g3, g4, mask;
  uint64_t f;

  if (st->leftover) { /* last partial block: append 1, pad with zeros */
    size_t i = st->leftover;
    st->buffer[i++] = 1;
    for (; i < 16; i++) st->buffer[i] = 0;
    st->final = 1;
    poly_blocks(st, st->buffer, 16);
  }

  h0 = st->h[0]; h1 = st->h[1]; h2 = st->h[2]; h3 = st->h[3]; h4 = st->h[4];

  /* fully carry h */
  c = h1 >> 26; h1 &= 0x3ffffffu;
  h2 += c; c = h2 >> 26; h2 &= 0x3ffffffu;
  h3 += c; c = h3 >> 26; h3 &= 0x3ffffffu;
  h4 += c; c = h4 >> 26; h4 &= 0x3ffffffu;
  h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffffu;
  h1 += c;

  /* g = h + -p = h - (2^130 - 5) */
  g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffffu;
  g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffffu;
  g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffffu;
  g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffffu;
  g4 = h4 + c - (1u << 26);

  /* h < p keeps h, else takes g; selected without branching */
  mask = (g4 >> 31) - 1u;
  g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
  mask = ~mask;
  h0 = (h0 & mask) | g0;
  h1 = (h1 & mask) | g1;
  h2 = (h2 & mask) | g2;
  h3 = (h3 & mask) | g3;
  h4 = (h4 & mask) | g4;

  /* h %= 2^128, packed into 32-bit words */
  h0 = h0 | (h1 << 26);
  h1 = (h1 >> 6) | (h2 << 20);
  h2 = (h2 >> 12) | (h3 << 14);
  h3 = (h3 >> 18) | (h4 << 8);

  /* tag = (h + s) % 2^128 */
  f = (uint64_t)h0 + st->pad[0]; h0 = (uint32_t)f;
  f = (uint64_t)h1 + st->pad[1] + (f >> 32); h1 = (uint32_t)f;
  f = (uint64_t)h2 + st->pad[2] + (f >> 32); h2 = (uint32_t)f;
  f = (uint64_t)h3 + st->pad[3] + (f >> 32); h3 = (uint32_t)f;

  st32(tag + 0, h0);
  st32(tag + 4, h1);
  st32(tag + 8, h2);
  st32(tag + 12, h3);

  asvault_wipe(st, sizeof *st);
}

void asvault_poly1305_mac(const uint8_t key[32], const uint8_t *m, size_t n,
                          uint8_t tag[16]) {
  asvault_poly1305 st;
  asvault_poly1305_init(&st, key);
  asvault_poly1305_update(&st, m, n);
  asvault_poly1305_final(&st, tag);
}

void asvault_poly1305_keygen(const uint8_t key[32], const uint8_t nonce[12],
                             uint8_t out[32]) {
  uint8_t block[64];
  asvault_chacha20_block(key, 0, nonce, block);
  memcpy(out, block, 32);
  asvault_wipe(block, sizeof block);
}

/* ---- AEAD (RFC 8439 2.8) ------------------------------------------------ */

static void aead_tag(const uint8_t otk[32], const uint8_t *aad, size_t aad_len,
                     const uint8_t *ct, size_t ct_len, uint8_t tag[16]) {
  static const uint8_t zeros[16] = {0};
  asvault_poly1305 st;
  uint8_t lens[16];
  asvault_poly1305_init(&st, otk);
  if (aad_len) asvault_poly1305_update(&st, aad, aad_len);
  if (aad_len % 16) asvault_poly1305_update(&st, zeros, 16 - aad_len % 16);
  if (ct_len) asvault_poly1305_update(&st, ct, ct_len);
  if (ct_len % 16) asvault_poly1305_update(&st, zeros, 16 - ct_len % 16);
  st64(lens, (uint64_t)aad_len);
  st64(lens + 8, (uint64_t)ct_len);
  asvault_poly1305_update(&st, lens, sizeof lens);
  asvault_poly1305_final(&st, tag);
}

void asvault_aead_seal(const uint8_t key[32], const uint8_t nonce[12],
                       const uint8_t *aad, size_t aad_len, const uint8_t *pt,
                       size_t len, uint8_t *out) {
  uint8_t otk[32];
  asvault_poly1305_keygen(key, nonce, otk);
  asvault_chacha20_xor(key, 1, nonce, pt, out, len);
  aead_tag(otk, aad, aad_len, out, len, out + len);
  asvault_wipe(otk, sizeof otk);
}

bool asvault_aead_open(const uint8_t key[32], const uint8_t nonce[12],
                       const uint8_t *aad, size_t aad_len, const uint8_t *in,
                       size_t in_len, uint8_t *out) {
  uint8_t otk[32], tag[16];
  size_t ct_len;
  bool ok;
  if (in_len < ASVAULT_TAG_SIZE) return false;
  ct_len = in_len - ASVAULT_TAG_SIZE;
  asvault_poly1305_keygen(key, nonce, otk);
  aead_tag(otk, aad, aad_len, in, ct_len, tag);
  ok = core_bytes_equal_ct(tag, in + ct_len, sizeof tag);
  asvault_wipe(otk, sizeof otk);
  asvault_wipe(tag, sizeof tag);
  if (!ok) return false;
  asvault_chacha20_xor(key, 1, nonce, in, out, ct_len);
  return true;
}

/* ---- PBKDF2-HMAC-SHA256 (RFC 8018 5.2) ---------------------------------- */

bool asvault_pbkdf2_sha256(const void *pw, size_t pw_len, const uint8_t *salt,
                           size_t salt_len, uint32_t iterations, uint8_t *out,
                           size_t out_len) {
  uint8_t first[128 + 4], u[32], next[32], t[32];
  uint32_t block = 1, j;
  size_t off = 0, n, k;

  if (iterations == 0 || salt_len > 128 || (!salt && salt_len) ||
      (!pw && pw_len) || (!out && out_len))
    return false;
  if (salt_len) memcpy(first, salt, salt_len);
  while (off < out_len) {
    /* U1 = PRF(P, S || INT(i)); T = U1 ^ U2 ^ ... ^ Uc */
    first[salt_len + 0] = (uint8_t)(block >> 24);
    first[salt_len + 1] = (uint8_t)(block >> 16);
    first[salt_len + 2] = (uint8_t)(block >> 8);
    first[salt_len + 3] = (uint8_t)block;
    core_hmac_sha256(pw, pw_len, first, salt_len + 4, u);
    memcpy(t, u, sizeof t);
    for (j = 1; j < iterations; j++) {
      core_hmac_sha256(pw, pw_len, u, sizeof u, next);
      memcpy(u, next, sizeof u);
      for (k = 0; k < sizeof t; k++) t[k] ^= u[k];
    }
    n = out_len - off < sizeof t ? out_len - off : sizeof t;
    memcpy(out + off, t, n);
    off += n;
    block++;
  }
  asvault_wipe(first, sizeof first);
  asvault_wipe(u, sizeof u);
  asvault_wipe(next, sizeof next);
  asvault_wipe(t, sizeof t);
  return true;
}
