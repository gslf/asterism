/*
 * test_util.c — buffers, strings, UTF-8, FNV-1a, Base64, token heuristic.
 */

#include "core_test.h"

#include <string.h>

TEST(buf_lifecycle) {
  core_buf b;
  char *s;
  core_buf_init(&b);
  ASSERT_OK(core_buf_appends(&b, "hello"));
  ASSERT_OK(core_buf_appendc(&b, ' '));
  ASSERT_OK(core_buf_printf(&b, "%s %d", "world", 42));
  ASSERT_EQ_STR(b.data, "hello world 42");
  ASSERT_EQ_INT((long long)b.len, 14);
  s = core_buf_detach(&b);
  ASSERT_EQ_STR(s, "hello world 42");
  ASSERT_EQ_INT((long long)b.len, 0);
  free(s);
  core_buf_free(&b);
}

TEST(buf_detach_empty) {
  core_buf b;
  char *s;
  core_buf_init(&b);
  s = core_buf_detach(&b);
  ASSERT_TRUE(s != NULL);
  ASSERT_EQ_STR(s, "");
  free(s);
  core_buf_free(&b);
}

TEST(slug_rules) {
  ASSERT_TRUE(core_slug_valid("thesis"));
  ASSERT_TRUE(core_slug_valid("a"));
  ASSERT_TRUE(core_slug_valid("a-1-b"));
  ASSERT_TRUE(!core_slug_valid(""));
  ASSERT_TRUE(!core_slug_valid("-lead"));
  ASSERT_TRUE(!core_slug_valid("Upper"));
  ASSERT_TRUE(!core_slug_valid("has space"));
  ASSERT_TRUE(!core_slug_valid("under_score"));
  {
    char big[70];
    memset(big, 'a', sizeof big);
    big[64] = '\0';
    ASSERT_TRUE(core_slug_valid(big));
    big[64] = 'a';
    big[65] = '\0';
    ASSERT_TRUE(!core_slug_valid(big));
  }
}

TEST(utf8_validation) {
  ASSERT_TRUE(core_utf8_valid("plain ascii", 11));
  ASSERT_TRUE(core_utf8_valid("caff\xC3\xA8", 6));
  ASSERT_TRUE(core_utf8_valid("\xE2\x9C\xA6", 3));      /* U+2726 */
  ASSERT_TRUE(core_utf8_valid("\xF0\x9F\x8C\x9F", 4));  /* U+1F31F */
  ASSERT_TRUE(!core_utf8_valid("\xC0\xAF", 2));         /* overlong */
  ASSERT_TRUE(!core_utf8_valid("\xED\xA0\x80", 3));     /* surrogate */
  ASSERT_TRUE(!core_utf8_valid("\xFF", 1));
  ASSERT_TRUE(!core_utf8_valid("\xC3", 1));             /* truncated */
}

TEST(fnv_vectors) {
  /* FNV-1a 64 known vectors */
  ASSERT_TRUE(core_fnv1a64("", 0) == 14695981039346656037ULL);
  ASSERT_TRUE(core_fnv1a64("a", 1) == 12638187200555641996ULL);
}

TEST(prefix_and_casecmp) {
  ASSERT_TRUE(core_str_has_prefix("asterism", "aster"));
  ASSERT_TRUE(!core_str_has_prefix("ast", "aster"));
  ASSERT_EQ_INT(core_strcasecmp("TeRsE", "terse"), 0);
  ASSERT_TRUE(core_strcasecmp("a", "b") != 0);
}

TEST(buf_grow) {
  core_buf b;
  int i;
  core_buf_init(&b);
  for (i = 0; i < 500; i++) ASSERT_OK(core_buf_appends(&b, "0123456789"));
  ASSERT_EQ_INT(b.len, 5000);
  ASSERT_EQ_INT(b.data[b.len], 0);
  ASSERT_TRUE(strncmp(b.data, "0123456789", 10) == 0);
  ASSERT_TRUE(strcmp(b.data + 4990, "0123456789") == 0);
  /* printf across the growth path */
  ASSERT_OK(core_buf_printf(&b, "%0512d", 7));
  ASSERT_EQ_INT(b.len, 5512);
  ASSERT_EQ_INT(b.data[5511], '7');
  core_buf_free(&b);
}

TEST(strdup_strndup) {
  char *s;
  ASSERT_TRUE(core_strdup(NULL) == NULL);
  s = core_strdup("hello");
  ASSERT_EQ_STR(s, "hello");
  free(s);
  s = core_strndup("hello", 3);
  ASSERT_EQ_STR(s, "hel");
  free(s);
  s = core_strndup("hi", 10);
  ASSERT_EQ_STR(s, "hi");
  free(s);
}

TEST(trim_blank) {
  char a[] = "  a b  ";
  char b[] = "\t\n x \r ";
  char c[] = "   ";
  char d[] = "xyz";
  ASSERT_EQ_STR(core_str_trim(a), "a b");
  ASSERT_EQ_STR(core_str_trim(b), "x");
  ASSERT_EQ_STR(core_str_trim(c), "");
  ASSERT_EQ_STR(core_str_trim(d), "xyz");
  ASSERT_TRUE(core_str_blank(NULL));
  ASSERT_TRUE(core_str_blank(""));
  ASSERT_TRUE(core_str_blank(" \t\r\n"));
  ASSERT_TRUE(!core_str_blank("a"));
  ASSERT_TRUE(!core_str_blank("  a  "));
}

TEST(base64_encode_vectors) {
  /* RFC 4648 §10 test vectors. */
  static const char *plain[] = {"", "f", "fo", "foo", "foob", "fooba",
                                "foobar"};
  static const char *coded[] = {"",     "Zg==", "Zm8=",     "Zm9v",
                                "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
  size_t i;
  for (i = 0; i < sizeof(plain) / sizeof(plain[0]); i++) {
    char *enc = core_base64_encode((const uint8_t *)plain[i],
                                    strlen(plain[i]));
    ASSERT_EQ_STR(enc, coded[i]);
    free(enc);
  }
}

TEST(base64_roundtrip_binary) {
  uint8_t raw[256];
  uint8_t *dec = NULL;
  size_t dec_len = 0, i;
  char *enc;
  for (i = 0; i < sizeof raw; i++) raw[i] = (uint8_t)i;
  enc = core_base64_encode(raw, sizeof raw);
  ASSERT_TRUE(enc != NULL);
  ASSERT_TRUE(core_base64_decode(enc, &dec, &dec_len));
  ASSERT_EQ_INT(dec_len, sizeof raw);
  ASSERT_TRUE(memcmp(dec, raw, sizeof raw) == 0);
  free(enc);
  free(dec);
}

TEST(base64_decode_vectors_and_rejects) {
  static const char *bad[] = {
      "Zg",       /* missing padding            */
      "Zg=",      /* short padding              */
      "A",        /* length % 4 == 1            */
      "AAAAA",    /* length % 4 == 1            */
      "Zm9v!",    /* invalid character          */
      "====",     /* padding only               */
      "Zg==Zg==", /* data after padding         */
      "Zm9v\n",   /* RFC 4648 without line wrap */
  };
  uint8_t *dec = NULL;
  size_t dec_len = 0, i;
  ASSERT_TRUE(core_base64_decode("Zm9vYmFy", &dec, &dec_len));
  ASSERT_EQ_INT(dec_len, 6);
  ASSERT_TRUE(memcmp(dec, "foobar", 6) == 0);
  free(dec);
  dec = NULL;
  ASSERT_TRUE(core_base64_decode("", &dec, &dec_len));
  ASSERT_EQ_INT(dec_len, 0);
  free(dec);
  for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    uint8_t *out = NULL;
    size_t out_len = 0;
    if (core_base64_decode(bad[i], &out, &out_len)) {
      TEST_FAILF("base64 decode accepted %s", bad[i]);
      free(out);
      return;
    }
  }
}

TEST(token_heuristic) {
  /* UTF-8 bytes / 4, minimum 1 for non-empty text. */
  ASSERT_EQ_INT(core_token_heuristic(""), 0);
  ASSERT_EQ_INT(core_token_heuristic("ab"), 1);
  ASSERT_EQ_INT(core_token_heuristic("abcd"), 1);
  ASSERT_EQ_INT(core_token_heuristic("abcdefgh"), 2);
  ASSERT_EQ_INT(core_token_heuristic("0123456789012345678901234567890123456789012"),
                10); /* 43 bytes */
  ASSERT_EQ_INT(core_token_heuristic("\xc3\xa8\xc3\xa8\xc3\xa8\xc3\xa8"),
                2); /* 4 two-byte code points = 8 bytes */
}

TEST(utf8_validation_and_character_count) {
  size_t n = 99;
  ASSERT_TRUE(core_utf8_count("hello", &n));
  ASSERT_EQ_INT(n, 5);
  ASSERT_TRUE(core_utf8_count("\xc3\xa8\xe2\x82\xac\xf0\x9f\x98\x80", &n));
  ASSERT_EQ_INT(n, 3);
  ASSERT_TRUE(!core_utf8_count("\xc0\x80", &n));       /* overlong NUL */
  ASSERT_TRUE(!core_utf8_count("\xed\xa0\x80", &n)); /* surrogate */
  ASSERT_TRUE(!core_utf8_count("\xf4\x90\x80\x80", &n));
  ASSERT_TRUE(!core_utf8_count("\xe2\x82", &n));      /* truncated */
}

TEST_LIST = {
    TEST_ENTRY(buf_lifecycle),
    TEST_ENTRY(buf_detach_empty),
    TEST_ENTRY(slug_rules),
    TEST_ENTRY(utf8_validation),
    TEST_ENTRY(fnv_vectors),
    TEST_ENTRY(prefix_and_casecmp),
    TEST_ENTRY(buf_grow),
    TEST_ENTRY(strdup_strndup),
    TEST_ENTRY(trim_blank),
    TEST_ENTRY(base64_encode_vectors),
    TEST_ENTRY(base64_roundtrip_binary),
    TEST_ENTRY(base64_decode_vectors_and_rejects),
    TEST_ENTRY(token_heuristic),
    TEST_ENTRY(utf8_validation_and_character_count),
};

RUN_ALL_TESTS()
