/*
 * test_redact.c — stored values (and simple encodings of them) back to
 * {{secret:NAME}}: as-is, base64 (whole and embedded), URL-encoded,
 * JSON-escaped; overlapping and nested values, look-alike names, short
 * values, and the resolve -> redact round trip.
 */

#include "asvault_test.h"

#include "asvault_internal.h"

typedef struct {
  char base[256];
  char dir[320];
  asvault *v;
} fixture;

static const char *const T_ANY[] = {"net"};

static int put(asvault *v, const char *name, const char *value) {
  asvault_policy p;
  p.tools = T_ANY;
  p.n_tools = 1;
  p.domains = NULL;
  p.n_domains = 0;
  return asvault_set(v, name, value, &p) == ASVAULT_OK;
}

#define API_KEY "sk-live-abcdef0123456789"
#define PW      "p@ss w/rd+\"x\\y"
#define UNI     "p\xc3\xa4ssw\xc3\xb6rd\xe2\x82\xac-\xf0\x9f\x98\x80"

static int fx_init(fixture *f) {
#if !defined(_WIN32)
  unsetenv(ASVAULT_PASSPHRASE_ENV);
#endif
  if (!test_tmpdir(f->base)) return 0;
  snprintf(f->dir, sizeof f->dir, "%s/vault", f->base);
  if (asvault_open(f->dir, &f->v) != ASVAULT_OK) return 0;
  return put(f->v, "api-key", API_KEY) && put(f->v, "pw", PW) &&
         put(f->v, "pin", "1234") && put(f->v, "uni", UNI);
}

static void fx_done(fixture *f) {
  asvault_close(f->v);
  test_rmtree(f->base);
}

/* Redact text and compare with want. */
static int redacts_to(asvault *v, const char *text, const char *want) {
  char *out = NULL;
  int ok;
  if (asvault_redact(v, text, &out) != ASVAULT_OK) {
    fprintf(stderr, "  redact failed: %s\n", asvault_last_error(v));
    return 0;
  }
  ok = strcmp(out, want) == 0;
  if (!ok) fprintf(stderr, "  got:  %s\n  want: %s\n", out, want);
  asvault_free_secret(out);
  return ok;
}

TEST(plain_values) {
  fixture f;
  ASSERT_TRUE(fx_init(&f));
  ASSERT_TRUE(redacts_to(f.v, "token=" API_KEY " end",
                         "token={{secret:api-key}} end"));
  ASSERT_TRUE(redacts_to(f.v, API_KEY API_KEY,
                         "{{secret:api-key}}{{secret:api-key}}"));
  ASSERT_TRUE(redacts_to(f.v, API_KEY, "{{secret:api-key}}"));
  ASSERT_TRUE(redacts_to(f.v, "a " PW " b", "a {{secret:pw}} b"));
  ASSERT_TRUE(redacts_to(f.v, "x" UNI "y", "x{{secret:uni}}y"));
  /* near misses stay */
  ASSERT_TRUE(redacts_to(f.v, "sk-live-abcdef012345678",
                         "sk-live-abcdef012345678"));
  ASSERT_TRUE(redacts_to(f.v, "", ""));
  /* values under ASVAULT_REDACT_MIN bytes are not redacted */
  ASSERT_TRUE(redacts_to(f.v, "pin 1234", "pin 1234"));
  /* already redacted text is left as it is */
  ASSERT_TRUE(redacts_to(f.v, "{{secret:api-key}}", "{{secret:api-key}}"));
  fx_done(&f);
}

TEST(base64_values) {
  fixture f;
  char *b64, *url, text[512];
  size_t i, n;
  ASSERT_TRUE(fx_init(&f));
  b64 = core_base64_encode((const uint8_t *)PW, strlen(PW));
  ASSERT_TRUE(b64 != NULL);
  snprintf(text, sizeof text, "auth: %s;", b64);
  ASSERT_TRUE(redacts_to(f.v, text, "auth: {{secret:pw}};"));
  /* unpadded */
  n = strlen(b64);
  while (n > 0 && b64[n - 1] == '=') n--;
  ASSERT_TRUE(n < strlen(b64)); /* this value does pad */
  snprintf(text, sizeof text, "[%.*s]", (int)n, b64);
  ASSERT_TRUE(redacts_to(f.v, text, "[{{secret:pw}}]"));
  /* URL-safe alphabet */
  url = core_strdup(b64);
  ASSERT_TRUE(url != NULL);
  for (i = 0; url[i]; i++) {
    if (url[i] == '+') url[i] = '-';
    if (url[i] == '/') url[i] = '_';
  }
  snprintf(text, sizeof text, "%s", url);
  ASSERT_TRUE(redacts_to(f.v, text, "{{secret:pw}}"));
  free(url);
  free(b64);

  /* inside a longer encoding, at each of the three alignments */
  for (i = 0; i < 3; i++) {
    char plain[128];
    char *enc, *out = NULL;
    snprintf(plain, sizeof plain, "%.*suser:%s:tail", (int)i, "xy", API_KEY);
    enc = core_base64_encode((const uint8_t *)plain, strlen(plain));
    ASSERT_TRUE(enc != NULL);
    snprintf(text, sizeof text, "Authorization: Basic %s", enc);
    ASSERT_OK(asvault_redact(f.v, text, &out));
    ASSERT_TRUE(strstr(out, "{{secret:api-key}}") != NULL);
    ASSERT_TRUE(strstr(out, enc) == NULL);
    /* what survives around the placeholder is at most the bytes that
     * straddle the value: 'user:' + 'tail' + 2 chars of mixing */
    ASSERT_TRUE(strlen(out) - strlen("{{secret:api-key}}") <
                strlen("Authorization: Basic ") + 20);
    asvault_free_secret(out);
    free(enc);
  }
  fx_done(&f);
}

TEST(url_encoded_values) {
  fixture f;
  ASSERT_TRUE(fx_init(&f));
  /* every reserved byte encoded, either hex case */
  ASSERT_TRUE(redacts_to(f.v, "?p=p%40ss%20w%2Frd%2B%22x%5Cy&x=1",
                         "?p={{secret:pw}}&x=1"));
  ASSERT_TRUE(redacts_to(f.v, "?p=p%40ss%20w%2frd%2b%22x%5cy&x=1",
                         "?p={{secret:pw}}&x=1"));
  /* form encoding, '+' for space */
  ASSERT_TRUE(redacts_to(f.v, "p=p%40ss+w%2Frd%2B%22x%5Cy",
                         "p={{secret:pw}}"));
  /* partial encodings (encodeURIComponent, Python quote with safe='/') */
  ASSERT_TRUE(redacts_to(f.v, "p=p%40ss%20w%2Frd%2B%22x%5Cy",
                         "p={{secret:pw}}"));
  ASSERT_TRUE(redacts_to(f.v, "p=p%40ss%20w/rd%2B%22x%5Cy", "p={{secret:pw}}"));
  /* even unreserved bytes encoded */
  ASSERT_TRUE(redacts_to(f.v, "%73%6B-live-abcdef0123456789",
                         "{{secret:api-key}}"));
  /* UTF-8 bytes */
  ASSERT_TRUE(redacts_to(f.v, "p%C3%A4ssw%C3%B6rd%E2%82%AC-%F0%9F%98%80!",
                         "{{secret:uni}}!"));
  fx_done(&f);
}

TEST(json_escaped_values) {
  fixture f;
  ASSERT_TRUE(fx_init(&f));
  ASSERT_TRUE(redacts_to(f.v, "{\"password\":\"p@ss w/rd+\\\"x\\\\y\"}",
                         "{\"password\":\"{{secret:pw}}\"}"));
  ASSERT_TRUE(redacts_to(f.v, "{\"password\":\"p@ss w\\/rd+\\\"x\\\\y\"}",
                         "{\"password\":\"{{secret:pw}}\"}"));
  ASSERT_TRUE(redacts_to(f.v, "{\"p\":\"p@ss w\\u002frd\\u002B\\u0022x\\u005cy\"}",
                         "{\"p\":\"{{secret:pw}}\"}"));
  /* ASCII-only JSON: \u escapes, a surrogate pair for the emoji */
  ASSERT_TRUE(redacts_to(f.v, "\"p\\u00e4ssw\\u00F6rd\\u20ac-\\ud83d\\ude00\"",
                         "\"{{secret:uni}}\""));
  /* a lone or mismatched surrogate is not a match */
  ASSERT_TRUE(redacts_to(f.v, "\"p\\u00e4ssw\\u00F6rd\\u20ac-\\ud83d\\u0041\"",
                         "\"p\\u00e4ssw\\u00F6rd\\u20ac-\\ud83d\\u0041\""));
  fx_done(&f);
}

TEST(values_with_escape_characters_in_them) {
  fixture f;
  ASSERT_TRUE(fx_init(&f));
  ASSERT_TRUE(put(f.v, "pct", "100%25-of-it"));
  ASSERT_TRUE(put(f.v, "bs", "a\\\\b-c\\nd"));
  ASSERT_TRUE(redacts_to(f.v, "[100%25-of-it]", "[{{secret:pct}}]"));
  ASSERT_TRUE(redacts_to(f.v, "[100%2525-of-it]", "[{{secret:pct}}]"));
  ASSERT_TRUE(redacts_to(f.v, "[a\\\\b-c\\nd]", "[{{secret:bs}}]"));
  ASSERT_TRUE(redacts_to(f.v, "[a\\\\\\\\b-c\\\\nd]", "[{{secret:bs}}]"));
  fx_done(&f);
}

TEST(overlapping_and_nested_values) {
  fixture f;
  ASSERT_TRUE(fx_init(&f));
  ASSERT_TRUE(put(f.v, "alpha", "abcdef123456"));
  ASSERT_TRUE(put(f.v, "beta", "123456ghijkl"));
  ASSERT_TRUE(put(f.v, "gamma", "xyz-abcdef123456-xyz"));
  ASSERT_TRUE(put(f.v, "prefix", "abcdef12"));
  /* the shared middle is covered once: nothing of either survives */
  ASSERT_TRUE(redacts_to(f.v, "<abcdef123456ghijkl>",
                         "<{{secret:alpha}}{{secret:beta}}>"));
  /* a value inside a longer one adds no placeholder of its own */
  ASSERT_TRUE(redacts_to(f.v, "<xyz-abcdef123456-xyz>", "<{{secret:gamma}}>"));
  /* a value that is a prefix of another: the longer match wins */
  ASSERT_TRUE(redacts_to(f.v, "<abcdef123456>", "<{{secret:alpha}}>"));
  ASSERT_TRUE(redacts_to(f.v, "<abcdef12>", "<{{secret:prefix}}>"));
  ASSERT_TRUE(redacts_to(f.v, "<abcdef1234>", "<{{secret:prefix}}34>"));
  /* identical values: reported once, under the first name */
  ASSERT_TRUE(put(f.v, "dup-b", "same-value-123"));
  ASSERT_TRUE(put(f.v, "dup-a", "same-value-123"));
  ASSERT_TRUE(redacts_to(f.v, "same-value-123", "{{secret:dup-a}}"));
  fx_done(&f);
}

TEST(look_alike_names) {
  fixture f;
  ASSERT_TRUE(fx_init(&f));
  ASSERT_TRUE(put(f.v, "api", "short-api-value"));
  ASSERT_TRUE(put(f.v, "api-key-2", "second-api-value"));
  ASSERT_TRUE(redacts_to(f.v, API_KEY " short-api-value second-api-value",
                         "{{secret:api-key}} {{secret:api}} "
                         "{{secret:api-key-2}}"));
  fx_done(&f);
}

TEST(resolve_then_redact_round_trip) {
  fixture f;
  char *resolved = NULL, *back = NULL;
  const char *text =
      "curl -H 'Authorization: Bearer {{secret:api-key}}' -u 'me:{{secret:pw}}'";
  ASSERT_TRUE(fx_init(&f));
  ASSERT_OK(asvault_resolve(f.v, text, "net", "api.example.com", &resolved));
  ASSERT_TRUE(strstr(resolved, API_KEY) != NULL);
  ASSERT_OK(asvault_redact(f.v, resolved, &back));
  ASSERT_EQ_STR(back, text);
  asvault_free_secret(resolved);
  asvault_free_secret(back);
  fx_done(&f);
}

TEST(redact_without_vault_or_entries) {
  char base[256], dir[300], *out = NULL;
  asvault *v = NULL;
  ASSERT_TRUE(test_tmpdir(base));
  snprintf(dir, sizeof dir, "%s/vault", base);
  ASSERT_OK(asvault_open(dir, &v));
  ASSERT_OK(asvault_redact(v, "nothing " API_KEY, &out));
  ASSERT_EQ_STR(out, "nothing " API_KEY);
  asvault_free_secret(out);
  ASSERT_TRUE(put(v, "k", API_KEY));
  ASSERT_OK(asvault_delete(v, "k"));
  ASSERT_OK(asvault_redact(v, API_KEY, &out)); /* vault without entries */
  ASSERT_EQ_STR(out, API_KEY);
  asvault_free_secret(out);
  ASSERT_ERR(asvault_redact(v, NULL, &out), ASVAULT_ERR_INVALID);
  ASSERT_ERR(asvault_redact(v, "x", NULL), ASVAULT_ERR_INVALID);
  asvault_close(v);
  test_rmtree(base);
}

TEST(redact_fails_closed_on_a_tampered_entry) {
  fixture f;
  char file[400], *text, *ct, *out = NULL;
  FILE *fp;
  ASSERT_TRUE(fx_init(&f));
  snprintf(file, sizeof file, "%s/vault.xcdn", f.dir);
  text = test_read_file(file, NULL);
  ASSERT_TRUE(text != NULL);
  ct = strstr(text, "ciphertext: b\"");
  ASSERT_TRUE(ct != NULL);
  ct[14] = ct[14] == 'A' ? 'B' : 'A';
  fp = fopen(file, "wb");
  ASSERT_TRUE(fp != NULL);
  fputs(text, fp);
  fclose(fp);
  free(text);
  ASSERT_ERR(asvault_redact(f.v, "x " API_KEY, &out), ASVAULT_ERR_CRYPTO);
  ASSERT_TRUE(out == NULL);
  fx_done(&f);
}

TEST(large_output_is_redacted) {
  fixture f;
  size_t n = 1u << 20, i;
  char *big, *out = NULL;
  ASSERT_TRUE(fx_init(&f));
  big = (char *)malloc(n + 1);
  ASSERT_TRUE(big != NULL);
  for (i = 0; i < n; i++) big[i] = (char)('a' + (i * 7) % 26);
  memcpy(big + 1000, API_KEY, strlen(API_KEY));
  memcpy(big + n - 100, PW, strlen(PW));
  big[n] = '\0';
  ASSERT_OK(asvault_redact(f.v, big, &out));
  ASSERT_TRUE(strstr(out, API_KEY) == NULL);
  ASSERT_TRUE(strstr(out, PW) == NULL);
  ASSERT_TRUE(strstr(out, "{{secret:api-key}}") != NULL);
  ASSERT_TRUE(strstr(out, "{{secret:pw}}") != NULL);
  asvault_free_secret(out);
  free(big);
  fx_done(&f);
}

TEST_LIST = {
    TEST_ENTRY(plain_values),
    TEST_ENTRY(base64_values),
    TEST_ENTRY(url_encoded_values),
    TEST_ENTRY(json_escaped_values),
    TEST_ENTRY(values_with_escape_characters_in_them),
    TEST_ENTRY(overlapping_and_nested_values),
    TEST_ENTRY(look_alike_names),
    TEST_ENTRY(resolve_then_redact_round_trip),
    TEST_ENTRY(redact_without_vault_or_entries),
    TEST_ENTRY(redact_fails_closed_on_a_tampered_entry),
    TEST_ENTRY(large_output_is_redacted),
};

RUN_ALL_TESTS()
