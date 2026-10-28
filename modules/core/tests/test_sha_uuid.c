/*
 * test_sha_uuid.c — SHA-256 vectors, streaming, files; UUID v4.
 */

#include "core_test.h"

#include <string.h>


TEST(sha256_fips_vectors) {
  uint8_t hash[32];
  char hex[65];
  core_sha256("abc", 3, hash);
  core_sha256_hex(hash, 32, hex);
  ASSERT_EQ_STR(hex,
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  core_sha256("", 0, hash);
  core_sha256_hex(hash, 32, hex);
  ASSERT_EQ_STR(hex,
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(sha256_streaming_matches_one_shot) {
  uint8_t a[32], b[32];
  core_sha256_ctx ctx;
  const char *msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  char hex[65];
  core_sha256(msg, strlen(msg), a);
  core_sha256_init(&ctx);
  core_sha256_update(&ctx, msg, 10);
  core_sha256_update(&ctx, msg + 10, strlen(msg) - 10);
  core_sha256_final(&ctx, b);
  ASSERT_TRUE(memcmp(a, b, 32) == 0);
  core_sha256_hex(a, 32, hex);
  ASSERT_EQ_STR(hex,
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(hmac_sha256_rfc4231_vectors) {
  uint8_t key[131], mac[32];
  char hex[65];
  memset(key, 0x0b, 20);
  core_hmac_sha256(key, 20, "Hi There", 8, mac);
  core_sha256_hex(mac, 32, hex);
  ASSERT_EQ_STR(hex,
      "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  core_hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, mac);
  core_sha256_hex(mac, 32, hex);
  ASSERT_EQ_STR(hex,
      "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  /* Test case 6: a key longer than the block is hashed first. */
  memset(key, 0xaa, sizeof key);
  core_hmac_sha256(key, sizeof key,
      "Test Using Larger Than Block-Size Key - Hash Key First", 54, mac);
  core_sha256_hex(mac, 32, hex);
  ASSERT_EQ_STR(hex,
      "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST(bytes_equal_ct) {
  ASSERT_TRUE(core_bytes_equal_ct("abcd", "abcd", 4));
  ASSERT_TRUE(!core_bytes_equal_ct("abcd", "abce", 4));
  ASSERT_TRUE(core_bytes_equal_ct("", "", 0));
}

TEST(sha256_hex_prefix) {
  uint8_t hash[32];
  char hex[9];
  core_sha256("abc", 3, hash);
  core_sha256_hex(hash, 4, hex);
  ASSERT_EQ_STR(hex, "ba7816bf");
}

TEST(sha256_file_matches_buffer) {
  char dir[256], path[300];
  uint8_t buf[100000];
  uint8_t want[32], got[32];
  size_t i;
  FILE *f;
  ASSERT_TRUE(test_tmpdir(dir));
  snprintf(path, sizeof path, "%s/blob.bin", dir);
  for (i = 0; i < sizeof buf; i++)
    buf[i] = (uint8_t)(i * 31u + 7u);
  f = fopen(path, "wb");
  ASSERT_TRUE(f != NULL);
  ASSERT_EQ_INT((long long)fwrite(buf, 1, sizeof buf, f),
                (long long)sizeof buf);
  fclose(f);
  core_sha256(buf, sizeof buf, want);
  ASSERT_OK(core_sha256_file(path, got));
  ASSERT_TRUE(memcmp(want, got, 32) == 0);
  test_rmtree(dir);
}

TEST(sha256_file_missing_errors) {
  uint8_t out[32];
  char dir[256], path[300];
  ASSERT_TRUE(test_tmpdir(dir));
  snprintf(path, sizeof path, "%s/no-such-file.bin", dir);
  ASSERT_ERR(core_sha256_file(path, out), CORE_ERR_NOT_FOUND);
  ASSERT_ERR(core_sha256_file(NULL, out), CORE_ERR_INVALID);
  test_rmtree(dir);
}

TEST(uuid_v4_shape) {
  char a[37], b[37];
  size_t i;
  core_uuid_v4(a);
  core_uuid_v4(b);
  ASSERT_EQ_INT((long long)strlen(a), 36);
  ASSERT_EQ_INT((long long)strlen(b), 36);
  for (i = 0; i < 36; i++) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      ASSERT_TRUE(a[i] == '-');
    } else {
      ASSERT_TRUE((a[i] >= '0' && a[i] <= '9') ||
                  (a[i] >= 'a' && a[i] <= 'f'));
    }
  }
  ASSERT_TRUE(a[14] == '4');                       /* version nibble    */
  ASSERT_TRUE(a[19] == '8' || a[19] == '9' ||
              a[19] == 'a' || a[19] == 'b');       /* RFC 4122 variant  */
  ASSERT_TRUE(b[14] == '4');
  ASSERT_TRUE(strcmp(a, b) != 0);                  /* two v4s differ    */
}

TEST(uuid_valid_accept_reject) {
  ASSERT_TRUE(core_uuid_valid("550e8400-e29b-41d4-a716-446655440000"));
  {
    char u[37];
    core_uuid_v4(u);
    ASSERT_TRUE(core_uuid_valid(u));
  }
  ASSERT_TRUE(!core_uuid_valid(NULL));
  ASSERT_TRUE(!core_uuid_valid(""));
  ASSERT_TRUE(!core_uuid_valid("550e8400-e29b-41d4-a716-44665544000"));
  ASSERT_TRUE(!core_uuid_valid("550e8400-e29b-41d4-a716-4466554400000"));
  ASSERT_TRUE(!core_uuid_valid("550e8400-e29b-41d4-a716_446655440000"));
  ASSERT_TRUE(!core_uuid_valid("550e8400-e29b-41d4-a716-44665544000g"));
  ASSERT_TRUE(!core_uuid_valid("550e8400e29b-41d4-a716-4466554400000"));
}

TEST(uuid_bytes_roundtrip) {
  static const char *text = "550e8400-e29b-41d4-a716-446655440000";
  uint8_t raw[16];
  char back[37];
  ASSERT_TRUE(core_uuid_to_bytes(text, raw));
  ASSERT_EQ_INT(raw[0], 0x55);
  ASSERT_EQ_INT(raw[1], 0x0e);
  ASSERT_EQ_INT(raw[15], 0x00);
  core_uuid_from_bytes(raw, back);
  ASSERT_EQ_STR(back, text);
  {
    char u[37], u2[37];
    uint8_t b[16];
    core_uuid_v4(u);
    ASSERT_TRUE(core_uuid_to_bytes(u, b));
    core_uuid_from_bytes(b, u2);
    ASSERT_EQ_STR(u2, u);
  }
  ASSERT_TRUE(!core_uuid_to_bytes("not-a-uuid", raw));
}

TEST(sha256_million_a) {
  core_sha256_ctx ctx;
  uint8_t d[32];
  char block[1000];
  int i;
  memset(block, 'a', sizeof block);
  core_sha256_init(&ctx);
  for (i = 0; i < 1000; i++) core_sha256_update(&ctx, block, sizeof block);
  char hex[65];
  core_sha256_final(&ctx, d);
  core_sha256_hex(d, 32, hex);
  ASSERT_EQ_STR(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_LIST = {
    TEST_ENTRY(sha256_fips_vectors),
    TEST_ENTRY(sha256_streaming_matches_one_shot),
    TEST_ENTRY(hmac_sha256_rfc4231_vectors),
  TEST_ENTRY(bytes_equal_ct),
  TEST_ENTRY(sha256_hex_prefix),
    TEST_ENTRY(sha256_file_matches_buffer),
    TEST_ENTRY(sha256_file_missing_errors),
    TEST_ENTRY(uuid_v4_shape),
    TEST_ENTRY(uuid_valid_accept_reject),
    TEST_ENTRY(uuid_bytes_roundtrip),
    TEST_ENTRY(sha256_million_a),
};

RUN_ALL_TESTS()
