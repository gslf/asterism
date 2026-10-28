/*
 * test_crypto.c — ChaCha20, Poly1305 and the AEAD against RFC 8439
 * (sections 2.1.1, 2.2.1, 2.3.2, 2.4.2, 2.5.2, 2.6.2, 2.8.2 and Appendix
 * A.1, A.3), PBKDF2-HMAC-SHA256 against RFC 7914 section 11 and the
 * widely used RFC 6070-style SHA-256 vectors.
 */

#include "asvault_test.h"

#include "asvault_internal.h"

/* Hex to bytes, skipping spaces, colons and newlines. */
static size_t unhex(const char *hex, uint8_t *out, size_t cap) {
  size_t n = 0;
  int hi = -1;
  for (; *hex; hex++) {
    int v;
    char c = *hex;
    if (c == ' ' || c == ':' || c == '\n') continue;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else return (size_t)-1;
    if (hi < 0) {
      hi = v;
    } else {
      if (n >= cap) return (size_t)-1;
      out[n++] = (uint8_t)(hi * 16 + v);
      hi = -1;
    }
  }
  return hi < 0 ? n : (size_t)-1;
}

#define ASSERT_HEX(buf, len, hex)                                         \
  do {                                                                    \
    uint8_t want_[512];                                                   \
    size_t wn_ = unhex((hex), want_, sizeof want_);                       \
    ASSERT_EQ_INT(wn_, (len));                                            \
    ASSERT_TRUE(memcmp((buf), want_, wn_) == 0);                          \
  } while (0)

static void seq_bytes(uint8_t *out, size_t n, uint8_t first) {
  size_t i;
  for (i = 0; i < n; i++) out[i] = (uint8_t)(first + i);
}

static const char SUNSCREEN[] =
    "Ladies and Gentlemen of the class of '99: If I could offer you only one "
    "tip for the future, sunscreen would be it.";

/* ---- ChaCha20 ----------------------------------------------------------- */

TEST(chacha_quarter_round_2_1_1) {
  uint32_t x[16] = {0};
  x[0] = 0x11111111u;
  x[1] = 0x01020304u;
  x[2] = 0x9b8d6f43u;
  x[3] = 0x01234567u;
  asvault_chacha20_quarter_round(x, 0, 1, 2, 3);
  ASSERT_EQ_INT(x[0], 0xea2a92f4u);
  ASSERT_EQ_INT(x[1], 0xcb1cf8ceu);
  ASSERT_EQ_INT(x[2], 0x4581472eu);
  ASSERT_EQ_INT(x[3], 0x5881c4bbu);
}

TEST(chacha_quarter_round_on_state_2_2_1) {
  uint32_t x[16] = {
      0x879531e0u, 0xc5ecf37du, 0x516461b1u, 0xc9a62f8au,
      0x44c20ef3u, 0x3390af7fu, 0xd9fc690bu, 0x2a5f714cu,
      0x53372767u, 0xb00a5631u, 0x974c541au, 0x359e9963u,
      0x5c971061u, 0x3d631689u, 0x2098d9d6u, 0x91dbd320u};
  const uint32_t want[16] = {
      0x879531e0u, 0xc5ecf37du, 0xbdb886dcu, 0xc9a62f8au,
      0x44c20ef3u, 0x3390af7fu, 0xd9fc690bu, 0xcfacafd2u,
      0xe46bea80u, 0xb00a5631u, 0x974c541au, 0x359e9963u,
      0x5c971061u, 0xccc07c79u, 0x2098d9d6u, 0x91dbd320u};
  size_t i;
  asvault_chacha20_quarter_round(x, 2, 7, 8, 13);
  for (i = 0; i < 16; i++) ASSERT_EQ_INT(x[i], want[i]);
}

TEST(chacha_block_2_3_2) {
  uint8_t key[32], nonce[12], out[64];
  seq_bytes(key, 32, 0);
  ASSERT_EQ_INT(unhex("000000090000004a00000000", nonce, 12), 12);
  asvault_chacha20_block(key, 1, nonce, out);
  ASSERT_HEX(out, 64,
             "10 f1 e7 e4 d1 3b 59 15 50 0f dd 1f a3 20 71 c4"
             "c7 d1 f4 c7 33 c0 68 03 04 22 aa 9a c3 d4 6c 4e"
             "d2 82 64 46 07 9f aa 09 14 c2 d7 05 d9 8b 02 a2"
             "b5 12 9c d1 de 16 4e b9 cb d0 83 e8 a2 50 3c 4e");
}

TEST(chacha_block_appendix_a1) {
  uint8_t key[32] = {0}, nonce[12] = {0}, out[64];
  asvault_chacha20_block(key, 0, nonce, out);
  ASSERT_HEX(out, 64,
             "76 b8 e0 ad a0 f1 3d 90 40 5d 6a e5 53 86 bd 28"
             "bd d2 19 b8 a0 8d ed 1a a8 36 ef cc 8b 77 0d c7"
             "da 41 59 7c 51 57 48 8d 77 24 e0 3f b8 d8 4a 37"
             "6a 43 b8 f4 15 18 a1 1c c3 87 b6 69 b2 ee 65 86");
  asvault_chacha20_block(key, 1, nonce, out);
  ASSERT_HEX(out, 64,
             "9f 07 e7 be 55 51 38 7a 98 ba 97 7c 73 2d 08 0d"
             "cb 0f 29 a0 48 e3 65 69 12 c6 53 3e 32 ee 7a ed"
             "29 b7 21 76 9c e6 4e 43 d5 71 33 b0 74 d8 39 d5"
             "31 ed 1f 28 51 0a fb 45 ac e1 0a 1f 4b 79 4d 6f");
}

TEST(chacha_encrypt_2_4_2) {
  uint8_t key[32], nonce[12], ct[sizeof SUNSCREEN], back[sizeof SUNSCREEN];
  size_t n = sizeof SUNSCREEN - 1;
  seq_bytes(key, 32, 0);
  ASSERT_EQ_INT(unhex("000000000000004a00000000", nonce, 12), 12);
  ASSERT_EQ_INT(n, 114);
  asvault_chacha20_xor(key, 1, nonce, (const uint8_t *)SUNSCREEN, ct, n);
  ASSERT_HEX(ct, n,
             "6e 2e 35 9a 25 68 f9 80 41 ba 07 28 dd 0d 69 81"
             "e9 7e 7a ec 1d 43 60 c2 0a 27 af cc fd 9f ae 0b"
             "f9 1b 65 c5 52 47 33 ab 8f 59 3d ab cd 62 b3 57"
             "16 39 d6 24 e6 51 52 ab 8f 53 0c 35 9f 08 61 d8"
             "07 ca 0d bf 50 0d 6a 61 56 a3 8e 08 8a 22 b6 5e"
             "52 bc 51 4d 16 cc f8 06 81 8c e9 1a b7 79 37 36"
             "5a f9 0b bf 74 a3 5b e6 b4 0b 8e ed f2 78 5e 42"
             "87 4d");
  asvault_chacha20_xor(key, 1, nonce, ct, back, n);
  ASSERT_TRUE(memcmp(back, SUNSCREEN, n) == 0);
  /* in place */
  asvault_chacha20_xor(key, 1, nonce, back, back, n);
  ASSERT_TRUE(memcmp(back, ct, n) == 0);
}

/* ---- Poly1305 ----------------------------------------------------------- */

TEST(poly1305_2_5_2) {
  static const char msg[] = "Cryptographic Forum Research Group";
  uint8_t key[32], tag[16];
  asvault_poly1305 st;
  size_t i;
  ASSERT_EQ_INT(unhex("85:d6:be:78:57:55:6d:33:7f:44:52:fe:42:d5:06:a8"
                      "01:03:80:8a:fb:0d:b2:fd:4a:bf:f6:af:41:49:f5:1b",
                      key, 32),
                32);
  asvault_poly1305_mac(key, (const uint8_t *)msg, sizeof msg - 1, tag);
  ASSERT_HEX(tag, 16, "a8:06:1d:c1:30:51:36:c6:c2:2b:8b:af:0c:01:27:a9");
  /* the same tag through uneven incremental updates */
  memset(tag, 0, sizeof tag);
  asvault_poly1305_init(&st, key);
  for (i = 0; i < sizeof msg - 1; i += 3) {
    size_t n = sizeof msg - 1 - i < 3 ? sizeof msg - 1 - i : 3;
    asvault_poly1305_update(&st, (const uint8_t *)msg + i, n);
  }
  asvault_poly1305_final(&st, tag);
  ASSERT_HEX(tag, 16, "a8061dc1305136c6c22b8baf0c0127a9");
}

typedef struct {
  const char *key, *msg, *tag;
} poly_vec;

#define Z16 "00000000000000000000000000000000" /* 16 zero bytes */

/* Appendix A.3: #1 and the edge cases #5..#11 (carries, reduction). */
TEST(poly1305_appendix_a3) {
  static const poly_vec vecs[] = {
      {Z16 Z16, Z16 Z16 Z16 Z16, Z16}, /* #1: 64 zero bytes */
      {"02000000000000000000000000000000"
       "00000000000000000000000000000000",
       "ffffffffffffffffffffffffffffffff", "03000000000000000000000000000000"},
      {"02000000000000000000000000000000"
       "ffffffffffffffffffffffffffffffff",
       "02000000000000000000000000000000", "03000000000000000000000000000000"},
      {"01000000000000000000000000000000"
       "00000000000000000000000000000000",
       "ffffffffffffffffffffffffffffffff"
       "f0ffffffffffffffffffffffffffffff"
       "11000000000000000000000000000000",
       "05000000000000000000000000000000"},
      {"01000000000000000000000000000000"
       "00000000000000000000000000000000",
       "ffffffffffffffffffffffffffffffff"
       "fbfefefefefefefefefefefefefefefe"
       "01010101010101010101010101010101",
       "00000000000000000000000000000000"},
      {"02000000000000000000000000000000"
       "00000000000000000000000000000000",
       "fdffffffffffffffffffffffffffffff", "faffffffffffffffffffffffffffffff"},
      {"01000000000000000400000000000000"
       "00000000000000000000000000000000",
       "e33594d7505e43b90000000000000000"
       "3394d7505e4379cd0100000000000000"
       "00000000000000000000000000000000"
       "01000000000000000000000000000000",
       "14000000000000005500000000000000"},
      {"01000000000000000400000000000000"
       "00000000000000000000000000000000",
       "e33594d7505e43b90000000000000000"
       "3394d7505e4379cd0100000000000000"
       "00000000000000000000000000000000",
       "13000000000000000000000000000000"},
  };
  size_t i;
  for (i = 0; i < sizeof vecs / sizeof vecs[0]; i++) {
    uint8_t key[32], msg[128], tag[16], want[16];
    size_t mn = unhex(vecs[i].msg, msg, sizeof msg);
    ASSERT_EQ_INT(unhex(vecs[i].key, key, 32), 32);
    ASSERT_TRUE(mn != (size_t)-1);
    asvault_poly1305_mac(key, msg, mn, tag);
    ASSERT_EQ_INT(unhex(vecs[i].tag, want, 16), 16);
    if (memcmp(tag, want, 16) != 0) {
      TEST_FAILF("Appendix A.3 vector index %u", (unsigned)i);
      return;
    }
  }
}

TEST(poly1305_keygen_2_6_2) {
  uint8_t key[32], nonce[12], otk[32];
  seq_bytes(key, 32, 0x80);
  ASSERT_EQ_INT(unhex("000000000001020304050607", nonce, 12), 12);
  asvault_poly1305_keygen(key, nonce, otk);
  ASSERT_HEX(otk, 32,
             "8a d5 a0 8b 90 5f 81 cc 81 50 40 27 4a b2 94 71"
             "a8 33 b6 37 e3 fd 0d a5 08 db b8 e2 fd d1 a6 46");
}

/* ---- AEAD --------------------------------------------------------------- */

TEST(aead_seal_open_2_8_2) {
  uint8_t key[32], nonce[12], aad[12];
  uint8_t sealed[sizeof SUNSCREEN + 16], back[sizeof SUNSCREEN];
  size_t n = sizeof SUNSCREEN - 1;
  seq_bytes(key, 32, 0x80);
  ASSERT_EQ_INT(unhex("070000004041424344454647", nonce, 12), 12);
  ASSERT_EQ_INT(unhex("50515253c0c1c2c3c4c5c6c7", aad, 12), 12);
  asvault_aead_seal(key, nonce, aad, sizeof aad, (const uint8_t *)SUNSCREEN,
                    n, sealed);
  ASSERT_HEX(sealed, n,
             "d3 1a 8d 34 64 8e 60 db 7b 86 af bc 53 ef 7e c2"
             "a4 ad ed 51 29 6e 08 fe a9 e2 b5 a7 36 ee 62 d6"
             "3d be a4 5e 8c a9 67 12 82 fa fb 69 da 92 72 8b"
             "1a 71 de 0a 9e 06 0b 29 05 d6 a5 b6 7e cd 3b 36"
             "92 dd bd 7f 2d 77 8b 8c 98 03 ae e3 28 09 1b 58"
             "fa b3 24 e4 fa d6 75 94 55 85 80 8b 48 31 d7 bc"
             "3f f4 de f0 8e 4b 7a 9d e5 76 d2 65 86 ce c6 4b"
             "61 16");
  ASSERT_HEX(sealed + n, 16, "1a:e1:0b:59:4f:09:e2:6a:7e:90:2e:cb:d0:60:06:91");

  memset(back, 0, sizeof back);
  ASSERT_TRUE(asvault_aead_open(key, nonce, aad, sizeof aad, sealed, n + 16,
                                back));
  ASSERT_TRUE(memcmp(back, SUNSCREEN, n) == 0);
}

TEST(aead_rejects_any_change) {
  uint8_t key[32], nonce[12], aad[12], sealed[64 + 16], out[64];
  uint8_t pt[64];
  size_t i;
  seq_bytes(key, 32, 0x80);
  seq_bytes(nonce, 12, 1);
  seq_bytes(aad, 12, 0x50);
  seq_bytes(pt, 64, 7);
  asvault_aead_seal(key, nonce, aad, sizeof aad, pt, sizeof pt, sealed);
  for (i = 0; i < sizeof sealed; i++) { /* ciphertext and tag bytes */
    memset(out, 0xAA, sizeof out);
    sealed[i] ^= 0x01;
    ASSERT_TRUE(!asvault_aead_open(key, nonce, aad, sizeof aad, sealed,
                                   sizeof sealed, out));
    sealed[i] ^= 0x01;
    ASSERT_EQ_INT(out[0], 0xAA); /* nothing written on failure */
  }
  aad[3] ^= 0x80;
  ASSERT_TRUE(!asvault_aead_open(key, nonce, aad, sizeof aad, sealed,
                                 sizeof sealed, out));
  aad[3] ^= 0x80;
  ASSERT_TRUE(!asvault_aead_open(key, nonce, aad, sizeof aad - 1, sealed,
                                 sizeof sealed, out));
  nonce[11] ^= 1;
  ASSERT_TRUE(!asvault_aead_open(key, nonce, aad, sizeof aad, sealed,
                                 sizeof sealed, out));
  nonce[11] ^= 1;
  key[0] ^= 1;
  ASSERT_TRUE(!asvault_aead_open(key, nonce, aad, sizeof aad, sealed,
                                 sizeof sealed, out));
  key[0] ^= 1;
  ASSERT_TRUE(!asvault_aead_open(key, nonce, aad, sizeof aad, sealed, 15, out));
  ASSERT_TRUE(asvault_aead_open(key, nonce, aad, sizeof aad, sealed,
                                sizeof sealed, out));
  ASSERT_TRUE(memcmp(out, pt, sizeof pt) == 0);
}

TEST(aead_empty_plaintext_and_aad) {
  uint8_t key[32] = {0}, nonce[12] = {0}, sealed[16], out[1];
  asvault_aead_seal(key, nonce, NULL, 0, NULL, 0, sealed);
  ASSERT_TRUE(asvault_aead_open(key, nonce, NULL, 0, sealed, 16, out));
  sealed[0] ^= 1;
  ASSERT_TRUE(!asvault_aead_open(key, nonce, NULL, 0, sealed, 16, out));
}

/* ---- PBKDF2-HMAC-SHA256 ------------------------------------------------- */

typedef struct {
  const char *pw;
  size_t pw_len;
  const char *salt;
  size_t salt_len;
  uint32_t iterations;
  const char *dk;
} kdf_vec;

TEST(pbkdf2_sha256_vectors) {
  static const kdf_vec vecs[] = {
      /* RFC 7914 section 11 */
      {"passwd", 6, "salt", 4, 1,
       "55 ac 04 6e 56 e3 08 9f ec 16 91 c2 25 44 b6 05"
       "f9 41 85 21 6d de 04 65 e6 8b 9d 57 c2 0d ac bc"
       "49 ca 9c cc f1 79 b6 45 99 16 64 b3 9d 77 ef 31"
       "7c 71 b8 45 b1 e3 0b d5 09 11 20 41 d3 a1 97 83"},
      {"Password", 8, "NaCl", 4, 80000,
       "4d dc d8 f6 0b 98 be 21 83 0c ee 5e f2 27 01 f9"
       "64 1a 44 18 d0 4c 04 14 ae ff 08 87 6b 34 ab 56"
       "a1 d4 25 a1 22 58 33 54 9a db 84 1b 51 c9 b3 17"
       "6a 27 2b de bb a1 d0 78 47 8f 62 b3 97 f3 3c 8d"},
      /* RFC 6070 inputs with SHA-256 */
      {"password", 8, "salt", 4, 1,
       "12 0f b6 cf fc f8 b3 2c 43 e7 22 52 56 c4 f8 37"
       "a8 65 48 c9 2c cc 35 48 08 05 98 7c b7 0b e1 7b"},
      {"password", 8, "salt", 4, 2,
       "ae 4d 0c 95 af 6b 46 d3 2d 0a df f9 28 f0 6d d0"
       "2a 30 3f 8e f3 c2 51 df d6 e2 d8 5a 95 47 4c 43"},
      {"password", 8, "salt", 4, 4096,
       "c5 e4 78 d5 92 88 c8 41 aa 53 0d b6 84 5c 4c 8d"
       "96 28 93 a0 01 ce 4e 11 a4 96 38 73 aa 98 13 4a"},
      {"passwordPASSWORDpassword", 24,
       "saltSALTsaltSALTsaltSALTsaltSALTsalt", 36, 4096,
       "34 8c 89 db cb d3 2b 2f 32 d8 14 b8 11 6e 84 cf"
       "2b 17 34 7e bc 18 00 18 1c 4e 2a 1f b8 dd 53 e1"
       "c6 35 51 8c 7d ac 47 e9"},
      {"pass\0word", 9, "sa\0lt", 5, 4096,
       "89 b6 9d 05 16 f8 29 89 3c 69 62 26 65 0a 86 87"},
  };
  size_t i;
  for (i = 0; i < sizeof vecs / sizeof vecs[0]; i++) {
    uint8_t want[64], got[64];
    size_t n = unhex(vecs[i].dk, want, sizeof want);
    ASSERT_TRUE(n != (size_t)-1);
    ASSERT_TRUE(asvault_pbkdf2_sha256(vecs[i].pw, vecs[i].pw_len,
                                      (const uint8_t *)vecs[i].salt,
                                      vecs[i].salt_len, vecs[i].iterations,
                                      got, n));
    if (memcmp(got, want, n) != 0) {
      TEST_FAILF("PBKDF2 vector index %u", (unsigned)i);
      return;
    }
  }
}

TEST(pbkdf2_rejects_bad_parameters) {
  uint8_t out[32], salt[129] = {0};
  ASSERT_TRUE(!asvault_pbkdf2_sha256("pw", 2, salt, 16, 0, out, sizeof out));
  ASSERT_TRUE(!asvault_pbkdf2_sha256("pw", 2, salt, 129, 1, out, sizeof out));
  ASSERT_TRUE(asvault_pbkdf2_sha256("pw", 2, salt, 128, 1, out, sizeof out));
}

TEST(wipe_clears) {
  uint8_t b[33];
  size_t i;
  memset(b, 0x5A, sizeof b);
  asvault_wipe(b, sizeof b);
  for (i = 0; i < sizeof b; i++) ASSERT_EQ_INT(b[i], 0);
  asvault_wipe(NULL, 4); /* tolerated */
}

TEST_LIST = {
    TEST_ENTRY(chacha_quarter_round_2_1_1),
    TEST_ENTRY(chacha_quarter_round_on_state_2_2_1),
    TEST_ENTRY(chacha_block_2_3_2),
    TEST_ENTRY(chacha_block_appendix_a1),
    TEST_ENTRY(chacha_encrypt_2_4_2),
    TEST_ENTRY(poly1305_2_5_2),
    TEST_ENTRY(poly1305_appendix_a3),
    TEST_ENTRY(poly1305_keygen_2_6_2),
    TEST_ENTRY(aead_seal_open_2_8_2),
    TEST_ENTRY(aead_rejects_any_change),
    TEST_ENTRY(aead_empty_plaintext_and_aad),
    TEST_ENTRY(pbkdf2_sha256_vectors),
    TEST_ENTRY(pbkdf2_rejects_bad_parameters),
    TEST_ENTRY(wipe_clears),
};

RUN_ALL_TESTS()
