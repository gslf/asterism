/*
 * test_store.c — the vault on disk: round trip, policy normalization,
 * owner-only files, AAD binding (name, policy, ciphertext), corrupt files
 * never overwritten, key-file and passphrase modes, concurrency.
 */

#include "asvault_test.h"

#include "asvault_internal.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

typedef struct {
  char base[256];
  char dir[320];
  char file[352];
  char key[352];
} fixture;

static int fx_init(fixture *f) {
#if !defined(_WIN32)
  unsetenv(ASVAULT_PASSPHRASE_ENV);
#endif
  if (!test_tmpdir(f->base)) return 0;
  snprintf(f->dir, sizeof f->dir, "%s/root/vault", f->base);
  snprintf(f->file, sizeof f->file, "%s/vault.xcdn", f->dir);
  snprintf(f->key, sizeof f->key, "%s/vault.key", f->dir);
  return 1;
}

static void fx_done(fixture *f) {
  test_rmtree(f->base);
#if !defined(_WIN32)
  unsetenv(ASVAULT_PASSPHRASE_ENV);
#endif
}

static asvault_policy pol(const char *const *tools, size_t nt,
                          const char *const *domains, size_t nd) {
  asvault_policy p;
  p.tools = tools;
  p.n_tools = nt;
  p.domains = domains;
  p.n_domains = nd;
  return p;
}

static int write_text(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  size_t n = strlen(text);
  int ok;
  if (!f) return 0;
  ok = fwrite(text, 1, n, f) == n;
  return fclose(f) == 0 && ok;
}

/* Replace the first occurrence of from with to in the file. */
static int edit_file(const char *path, const char *from, const char *to) {
  char *text = test_read_file(path, NULL), *hit, *out;
  size_t n, fl = strlen(from), tl = strlen(to);
  int ok;
  if (!text) return 0;
  hit = strstr(text, from);
  if (!hit) {
    free(text);
    return 0;
  }
  n = strlen(text);
  out = (char *)malloc(n - fl + tl + 1);
  if (!out) {
    free(text);
    return 0;
  }
  memcpy(out, text, (size_t)(hit - text));
  memcpy(out + (hit - text), to, tl);
  strcpy(out + (hit - text) + tl, hit + fl);
  ok = write_text(path, out);
  free(out);
  free(text);
  return ok;
}

/* Swap the first two lines that start with prefix. */
static int swap_lines(const char *path, const char *prefix) {
  char *text = test_read_file(path, NULL), *a, *b, *ae, *be, *out, *o;
  size_t al, bl;
  int ok;
  if (!text) return 0;
  a = strstr(text, prefix);
  b = a ? strstr(a + 1, prefix) : NULL;
  if (!a || !b) {
    free(text);
    return 0;
  }
  ae = strchr(a, '\n');
  be = strchr(b, '\n');
  al = (size_t)(ae - a);
  bl = (size_t)(be - b);
  out = (char *)malloc(strlen(text) + 1);
  o = out;
  memcpy(o, text, (size_t)(a - text)); o += a - text;
  memcpy(o, b, bl); o += bl;
  memcpy(o, ae, (size_t)(b - ae)); o += b - ae;
  memcpy(o, a, al); o += al;
  strcpy(o, be);
  ok = write_text(path, out);
  free(out);
  free(text);
  return ok;
}

static const char *const IMAP[] = {"imap"};

/* ---- tests -------------------------------------------------------------- */

TEST(open_reads_and_creates_nothing) {
  fixture f;
  asvault *v = NULL;
  asvault_entry_info *list = NULL;
  size_t n = 7;
  char *value = NULL;
  ASSERT_TRUE(fx_init(&f));
  ASSERT_ERR(asvault_open(NULL, &v), ASVAULT_ERR_INVALID);
  ASSERT_ERR(asvault_open("", &v), ASVAULT_ERR_INVALID);
  ASSERT_OK(asvault_open(f.dir, &v));
  ASSERT_OK(asvault_list(v, &list, &n));
  ASSERT_EQ_INT(n, 0);
  asvault_list_free(list, n);
  ASSERT_ERR(asvault_get(v, "absent", "imap", &value), ASVAULT_ERR_NOT_FOUND);
  ASSERT_TRUE(value == NULL);
  ASSERT_ERR(asvault_delete(v, "absent"), ASVAULT_ERR_NOT_FOUND);
  ASSERT_TRUE(strstr(asvault_last_error(v), "absent") != NULL);
  ASSERT_TRUE(!os_file_exists(f.dir));
  asvault_close(v);
  fx_done(&f);
}

TEST(round_trip_set_get_list_delete) {
  static const char *const tools[] = {"telegram"};
  static const char *const imap_dom[] = {"imap.example.com"};
  fixture f;
  asvault *v = NULL, *w = NULL;
  asvault_policy p1 = pol(tools, 1, NULL, 0), p2 = pol(IMAP, 1, imap_dom, 1);
  asvault_entry_info *list = NULL;
  size_t n = 0;
  char *value = NULL, *text, created[32];
  ASSERT_TRUE(fx_init(&f));
  ASSERT_OK(asvault_open(f.dir, &v));
  ASSERT_OK(asvault_set(v, "telegram-bot", "123456:ABC-telegram-token", &p1));
  ASSERT_OK(asvault_set(v, "imap-password", "correct horse battery", &p2));

  ASSERT_OK(asvault_get(v, "telegram-bot", "telegram", &value));
  ASSERT_EQ_STR(value, "123456:ABC-telegram-token");
  asvault_free_secret(value);
  ASSERT_OK(asvault_get(v, "imap-password", "imap", &value));
  ASSERT_EQ_STR(value, "correct horse battery");
  asvault_free_secret(value);

  ASSERT_OK(asvault_list(v, &list, &n));
  ASSERT_EQ_INT(n, 2);
  ASSERT_EQ_STR(list[0].name, "imap-password");
  ASSERT_EQ_INT(list[0].n_tools, 1);
  ASSERT_EQ_STR(list[0].tools[0], "imap");
  ASSERT_EQ_INT(list[0].n_domains, 1);
  ASSERT_EQ_STR(list[0].domains[0], "imap.example.com");
  ASSERT_EQ_STR(list[1].name, "telegram-bot");
  ASSERT_EQ_INT(list[1].n_domains, 0);
  ASSERT_EQ_INT(strlen(list[1].updated_at), 20);
  asvault_list_free(list, n);

  /* the files never hold a value */
  text = test_read_file(f.file, NULL);
  ASSERT_TRUE(text != NULL);
  ASSERT_TRUE(strstr(text, "telegram-token") == NULL);
  ASSERT_TRUE(strstr(text, "horse") == NULL);
  ASSERT_TRUE(strstr(text, "#asvault") != NULL);
  ASSERT_TRUE(strstr(text, "mode: \"file\"") != NULL);
  {
    char *created_line = strstr(text, "created_at: t\"");
    char *entry = created_line ? strstr(created_line + 1, "created_at: t\"") : NULL;
    ASSERT_TRUE(entry != NULL);
    memcpy(created, entry + 14, 20);
    created[20] = '\0';
  }
  free(text);
  {
    size_t klen = 0;
    char *key = test_read_file(f.key, &klen);
    ASSERT_TRUE(key != NULL);
    ASSERT_EQ_INT(klen, 32);
    free(key);
  }

  /* a second handle sees the same vault; replacing keeps created_at */
  ASSERT_OK(asvault_open(f.dir, &w));
  ASSERT_OK(asvault_set(w, "imap-password", "new imap password", &p2));
  ASSERT_OK(asvault_get(v, "imap-password", "imap", &value));
  ASSERT_EQ_STR(value, "new imap password");
  asvault_free_secret(value);
  text = test_read_file(f.file, NULL);
  ASSERT_TRUE(text != NULL && strstr(text, created) != NULL);
  free(text);

  ASSERT_OK(asvault_delete(v, "imap-password"));
  ASSERT_ERR(asvault_get(w, "imap-password", "imap", &value),
             ASVAULT_ERR_NOT_FOUND);
  ASSERT_ERR(asvault_delete(w, "imap-password"), ASVAULT_ERR_NOT_FOUND);
  ASSERT_OK(asvault_list(w, &list, &n));
  ASSERT_EQ_INT(n, 1);
  asvault_list_free(list, n);
  ASSERT_OK(asvault_delete(w, "telegram-bot"));
  ASSERT_OK(asvault_list(w, &list, &n));
  ASSERT_EQ_INT(n, 0);
  asvault_list_free(list, n);
  asvault_close(w);
  asvault_close(v);
  fx_done(&f);
}

TEST(policy_is_normalized) {
  static const char *const tools[] = {"net", "imap", "net"};
  static const char *const doms[] = {"IMAP.Example.COM.", "*.Example.org",
                                     "imap.example.com"};
  fixture f;
  asvault *v = NULL;
  asvault_policy p = pol(tools, 3, doms, 3);
  asvault_entry_info *list = NULL;
  size_t n = 0;
  ASSERT_TRUE(fx_init(&f));
  ASSERT_OK(asvault_open(f.dir, &v));
  ASSERT_OK(asvault_set(v, "k", "value-123456", &p));
  ASSERT_OK(asvault_list(v, &list, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_INT(list[0].n_tools, 2);
  ASSERT_EQ_STR(list[0].tools[0], "imap");
  ASSERT_EQ_STR(list[0].tools[1], "net");
  ASSERT_EQ_INT(list[0].n_domains, 2);
  ASSERT_EQ_STR(list[0].domains[0], "*.example.org");
  ASSERT_EQ_STR(list[0].domains[1], "imap.example.com");
  asvault_list_free(list, n);
  asvault_close(v);
  fx_done(&f);
}

TEST(invalid_input_is_rejected_before_any_write) {
  static const char *const bad_tool[] = {"Imap"};
  static const char *const bad_dom[] = {"exa mple.com"};
  static const char *const bad_dom2[] = {"-x.example.com"};
  static const char *const star[] = {"*"};
  fixture f;
  asvault *v = NULL;
  asvault_policy ok = pol(IMAP, 1, NULL, 0);
  asvault_policy none = pol(NULL, 0, NULL, 0);
  asvault_policy bt = pol(bad_tool, 1, NULL, 0);
  asvault_policy bd = pol(IMAP, 1, bad_dom, 1);
  asvault_policy bd2 = pol(IMAP, 1, bad_dom2, 1);
  asvault_policy bd3 = pol(IMAP, 1, star, 1);
  char long_name[80], *big;
  ASSERT_TRUE(fx_init(&f));
  ASSERT_OK(asvault_open(f.dir, &v));
  memset(long_name, 'a', 65);
  long_name[65] = '\0';
  ASSERT_ERR(asvault_set(v, "Upper", "value-1", &ok), ASVAULT_ERR_INVALID);
  ASSERT_ERR(asvault_set(v, "-dash", "value-1", &ok), ASVAULT_ERR_INVALID);
  ASSERT_ERR(asvault_set(v, "", "value-1", &ok), ASVAULT_ERR_INVALID);
  ASSERT_ERR(asvault_set(v, NULL, "value-1", &ok), ASVAULT_ERR_INVALID);
  ASSERT_ERR(asvault_set(v, long_name, "value-1", &ok), ASVAULT_ERR_INVALID);
  ASSERT_ERR(asvault_set(v, "k", "", &ok), ASVAULT_ERR_INVALID);
  ASSERT_ERR(asvault_set(v, "k", NULL, &ok), ASVAULT_ERR_INVALID);
  ASSERT_ERR(asvault_set(v, "k", "bad \xff utf8", &ok), ASVAULT_ERR_INVALID);
  ASSERT_ERR(asvault_set(v, "k", "value-1", NULL), ASVAULT_ERR_INVALID);
  ASSERT_ERR(asvault_set(v, "k", "value-1", &none), ASVAULT_ERR_INVALID);
  ASSERT_TRUE(strstr(asvault_last_error(v), "tool") != NULL);
  ASSERT_ERR(asvault_set(v, "k", "value-1", &bt), ASVAULT_ERR_INVALID);
  ASSERT_TRUE(strstr(asvault_last_error(v), "Imap") != NULL);
  ASSERT_ERR(asvault_set(v, "k", "value-1", &bd), ASVAULT_ERR_INVALID);
  ASSERT_ERR(asvault_set(v, "k", "value-1", &bd2), ASVAULT_ERR_INVALID);
  ASSERT_ERR(asvault_set(v, "k", "value-1", &bd3), ASVAULT_ERR_INVALID);
  big = (char *)malloc(ASVAULT_VALUE_MAX + 2);
  ASSERT_TRUE(big != NULL);
  memset(big, 'x', ASVAULT_VALUE_MAX + 1);
  big[ASVAULT_VALUE_MAX + 1] = '\0';
  ASSERT_ERR(asvault_set(v, "k", big, &ok), ASVAULT_ERR_INVALID);
  big[ASVAULT_VALUE_MAX] = '\0'; /* exactly the maximum is fine */
  ASSERT_TRUE(!os_file_exists(f.dir));
  ASSERT_OK(asvault_set(v, "k", big, &ok));
  free(big);
  ASSERT_ERR(asvault_set(NULL, "k", "v", &ok), ASVAULT_ERR_INVALID);
  asvault_close(v);
  fx_done(&f);
}

#if !defined(_WIN32)
TEST(files_are_owner_only) {
  fixture f;
  asvault *v = NULL;
  asvault_policy p = pol(IMAP, 1, NULL, 0);
  struct stat st;
  char tmp[400];
  mode_t old = umask(022);
  ASSERT_TRUE(fx_init(&f));
  ASSERT_OK(asvault_open(f.dir, &v));
  ASSERT_OK(asvault_set(v, "k", "value-123456", &p));
  ASSERT_TRUE(stat(f.dir, &st) == 0);
  ASSERT_EQ_INT(st.st_mode & 0777, 0700);
  ASSERT_TRUE(stat(f.file, &st) == 0);
  ASSERT_EQ_INT(st.st_mode & 0777, 0600);
  ASSERT_TRUE(stat(f.key, &st) == 0);
  ASSERT_EQ_INT(st.st_mode & 0777, 0600);
  /* loosened by hand, tightened again by the next write */
  ASSERT_TRUE(chmod(f.dir, 0755) == 0);
  ASSERT_TRUE(chmod(f.file, 0644) == 0);
  ASSERT_OK(asvault_set(v, "k", "value-7654321", &p));
  ASSERT_TRUE(stat(f.dir, &st) == 0);
  ASSERT_EQ_INT(st.st_mode & 0777, 0700);
  ASSERT_TRUE(stat(f.file, &st) == 0);
  ASSERT_EQ_INT(st.st_mode & 0777, 0600);
  snprintf(tmp, sizeof tmp, "%s.tmp", f.file);
  ASSERT_TRUE(!os_file_exists(tmp));
  umask(old);
  asvault_close(v);
  fx_done(&f);
}
#endif

TEST(aad_binds_the_name) {
  static const char *const tools[] = {"imap"};
  fixture f;
  asvault *v = NULL;
  asvault_policy p = pol(tools, 1, NULL, 0);
  char *value = NULL;
  ASSERT_TRUE(fx_init(&f));
  ASSERT_OK(asvault_open(f.dir, &v));
  ASSERT_OK(asvault_set(v, "alpha", "alpha-secret-value", &p));
  ASSERT_OK(asvault_set(v, "bravo", "bravo-secret-value", &p));
  /* ciphertexts (with their nonces) moved between names */
  ASSERT_TRUE(swap_lines(f.file, "      ciphertext: "));
  ASSERT_TRUE(swap_lines(f.file, "      nonce: "));
  ASSERT_ERR(asvault_get(v, "alpha", "imap", &value), ASVAULT_ERR_CRYPTO);
  ASSERT_TRUE(strstr(asvault_last_error(v), "alpha") != NULL);
  ASSERT_ERR(asvault_get(v, "bravo", "imap", &value), ASVAULT_ERR_CRYPTO);
  ASSERT_TRUE(value == NULL);
  /* swapped back, both open again */
  ASSERT_TRUE(swap_lines(f.file, "      ciphertext: "));
  ASSERT_TRUE(swap_lines(f.file, "      nonce: "));
  ASSERT_OK(asvault_get(v, "alpha", "imap", &value));
  ASSERT_EQ_STR(value, "alpha-secret-value");
  asvault_free_secret(value);
  /* an entry renamed in the file */
  ASSERT_TRUE(edit_file(f.file, "name: \"alpha\"", "name: \"gamma\""));
  ASSERT_ERR(asvault_get(v, "gamma", "imap", &value), ASVAULT_ERR_CRYPTO);
  ASSERT_ERR(asvault_get(v, "alpha", "imap", &value), ASVAULT_ERR_NOT_FOUND);
  asvault_close(v);
  fx_done(&f);
}

TEST(aad_binds_the_policy) {
  static const char *const dom[] = {"imap.example.com"};
  fixture f;
  asvault *v = NULL;
  asvault_policy p = pol(IMAP, 1, dom, 1);
  char *value = NULL;
  ASSERT_TRUE(fx_init(&f));
  ASSERT_OK(asvault_open(f.dir, &v));
  ASSERT_OK(asvault_set(v, "pw", "imap-password-1", &p));
  /* granting another tool by editing the file does not work */
  ASSERT_TRUE(edit_file(f.file, "tools: [\"imap\"]", "tools: [\"imap\", \"net\"]"));
  ASSERT_ERR(asvault_get(v, "pw", "net", &value), ASVAULT_ERR_CRYPTO);
  ASSERT_ERR(asvault_get(v, "pw", "imap", &value), ASVAULT_ERR_CRYPTO);
  ASSERT_TRUE(edit_file(f.file, "tools: [\"imap\", \"net\"]", "tools: [\"imap\"]"));
  ASSERT_OK(asvault_get(v, "pw", "imap", &value));
  asvault_free_secret(value);
  /* neither does redirecting it to another domain, or dropping domains */
  ASSERT_TRUE(edit_file(f.file, "\"imap.example.com\"", "\"evil.example.net\""));
  ASSERT_ERR(asvault_get(v, "pw", "imap", &value), ASVAULT_ERR_CRYPTO);
  ASSERT_TRUE(edit_file(f.file, "domains: [\"evil.example.net\"]", "domains: []"));
  ASSERT_ERR(asvault_get(v, "pw", "imap", &value), ASVAULT_ERR_CRYPTO);
  ASSERT_ERR(asvault_resolve(v, "{{secret:pw}}", "imap", NULL, &value),
             ASVAULT_ERR_CRYPTO);
  ASSERT_TRUE(value == NULL);
  asvault_close(v);
  fx_done(&f);
}

TEST(tampered_ciphertext_fails_authentication) {
  fixture f;
  asvault *v = NULL;
  asvault_policy p = pol(IMAP, 1, NULL, 0);
  char *text, *ct, *value = NULL;
  ASSERT_TRUE(fx_init(&f));
  ASSERT_OK(asvault_open(f.dir, &v));
  ASSERT_OK(asvault_set(v, "pw", "imap-password-1", &p));
  text = test_read_file(f.file, NULL);
  ASSERT_TRUE(text != NULL);
  ct = strstr(text, "ciphertext: b\"");
  ASSERT_TRUE(ct != NULL);
  ct += 14;
  *ct = *ct == 'A' ? 'B' : 'A'; /* first base64 char: still canonical */
  ASSERT_TRUE(write_text(f.file, text));
  free(text);
  ASSERT_ERR(asvault_get(v, "pw", "imap", &value), ASVAULT_ERR_CRYPTO);
  ASSERT_TRUE(value == NULL);
  asvault_close(v);
  fx_done(&f);
}

TEST(corrupt_file_is_reported_and_never_overwritten) {
  static const char *const bad[] = {
      "this is not xcdn {",
      "#asvault { version: 2, created_at: t\"2026-01-01T00:00:00Z\", "
      "cipher: \"chacha20-poly1305\", key: { mode: \"file\", check: b\"\" }, "
      "entries: [] }",
      "#other { }",
      "#asvault { version: 1 }",
  };
  fixture f;
  asvault *v = NULL;
  asvault_policy p = pol(IMAP, 1, NULL, 0);
  asvault_entry_info *list = NULL;
  size_t n = 0, i;
  char *value = NULL, *text, *good;
  ASSERT_TRUE(fx_init(&f));
  ASSERT_OK(asvault_open(f.dir, &v));
  ASSERT_OK(asvault_set(v, "pw", "imap-password-1", &p));
  ASSERT_OK(asvault_set(v, "other", "other-password", &p));
  good = test_read_file(f.file, NULL);
  ASSERT_TRUE(good != NULL);
  for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    ASSERT_TRUE(write_text(f.file, bad[i]));
    ASSERT_ERR(asvault_set(v, "pw", "new-value-123", &p), ASVAULT_ERR_PARSE);
    ASSERT_TRUE(strstr(asvault_last_error(v), "corrupt") != NULL ||
                strstr(asvault_last_error(v), "version") != NULL);
    ASSERT_ERR(asvault_list(v, &list, &n), ASVAULT_ERR_PARSE);
    ASSERT_ERR(asvault_get(v, "pw", "imap", &value), ASVAULT_ERR_PARSE);
    ASSERT_ERR(asvault_delete(v, "pw"), ASVAULT_ERR_PARSE);
    text = test_read_file(f.file, NULL);
    ASSERT_EQ_STR(text, bad[i]);
    free(text);
  }
  /* structural damage inside a valid document */
  ASSERT_TRUE(write_text(f.file, good));
  ASSERT_TRUE(edit_file(f.file, "name: \"other\"", "name: \"pw\""));
  ASSERT_ERR(asvault_list(v, &list, &n), ASVAULT_ERR_PARSE);
  ASSERT_TRUE(strstr(asvault_last_error(v), "twice") != NULL);
  ASSERT_TRUE(write_text(f.file, good));
  ASSERT_TRUE(edit_file(f.file, "      nonce:", "      extra: 1,\n      nonce:"));
  ASSERT_ERR(asvault_list(v, &list, &n), ASVAULT_ERR_PARSE);
  ASSERT_TRUE(write_text(f.file, good));
  ASSERT_TRUE(edit_file(f.file, "mode: \"file\"", "mode: \"passphrase\""));
  ASSERT_ERR(asvault_get(v, "pw", "imap", &value), ASVAULT_ERR_PARSE);
  ASSERT_TRUE(write_text(f.file, good));
  ASSERT_OK(asvault_get(v, "pw", "imap", &value));
  asvault_free_secret(value);
  free(good);
  asvault_close(v);
  fx_done(&f);
}

TEST(key_file_missing_or_foreign_locks_the_vault) {
  fixture f;
  asvault *v = NULL, *w = NULL;
  asvault_policy p = pol(IMAP, 1, NULL, 0);
  uint8_t other[32];
  char *value = NULL, *before, *after;
  FILE *k;
  ASSERT_TRUE(fx_init(&f));
  ASSERT_OK(asvault_open(f.dir, &v));
  ASSERT_OK(asvault_set(v, "pw", "imap-password-1", &p));
  before = test_read_file(f.file, NULL);
  ASSERT_TRUE(remove(f.key) == 0);
  ASSERT_OK(asvault_open(f.dir, &w)); /* a handle without the cached key */
  ASSERT_ERR(asvault_get(w, "pw", "imap", &value), ASVAULT_ERR_LOCKED);
  ASSERT_TRUE(strstr(asvault_last_error(w), "missing") != NULL);
  /* never regenerated: that would orphan every entry */
  ASSERT_ERR(asvault_set(w, "new", "new-value-123", &p), ASVAULT_ERR_LOCKED);
  ASSERT_TRUE(!os_file_exists(f.key));
  after = test_read_file(f.file, NULL);
  ASSERT_EQ_STR(after, before);
  free(after);
  /* listing needs no key */
  {
    asvault_entry_info *list = NULL;
    size_t n = 0;
    ASSERT_OK(asvault_list(w, &list, &n));
    ASSERT_EQ_INT(n, 1);
    asvault_list_free(list, n);
  }
  memset(other, 0x42, sizeof other);
  k = fopen(f.key, "wb");
  ASSERT_TRUE(k != NULL);
  ASSERT_EQ_INT(fwrite(other, 1, sizeof other, k), sizeof other);
  fclose(k);
  ASSERT_ERR(asvault_get(w, "pw", "imap", &value), ASVAULT_ERR_LOCKED);
  ASSERT_TRUE(strstr(asvault_last_error(w), "does not belong") != NULL);
  k = fopen(f.key, "wb");
  ASSERT_TRUE(k != NULL);
  ASSERT_EQ_INT(fwrite(other, 1, 31, k), 31);
  fclose(k);
  ASSERT_ERR(asvault_get(w, "pw", "imap", &value), ASVAULT_ERR_LOCKED);
  ASSERT_TRUE(strstr(asvault_last_error(w), "32-byte") != NULL);
  ASSERT_TRUE(value == NULL);
  free(before);
  asvault_close(w);
  asvault_close(v);
  fx_done(&f);
}

TEST(leftover_key_file_and_temp_file_are_handled) {
  fixture f;
  asvault *v = NULL;
  asvault_policy p = pol(IMAP, 1, NULL, 0);
  uint8_t key[32];
  char tmp[400], *value = NULL, *after;
  size_t len = 0;
  FILE *k;
  ASSERT_TRUE(fx_init(&f));
  ASSERT_EQ_INT(os_mkdir_p(f.dir), ASVAULT_OK);
  memset(key, 0x17, sizeof key);
  k = fopen(f.key, "wb");
  ASSERT_TRUE(k != NULL);
  ASSERT_EQ_INT(fwrite(key, 1, sizeof key, k), sizeof key);
  fclose(k);
  snprintf(tmp, sizeof tmp, "%s.tmp", f.file);
  ASSERT_TRUE(write_text(tmp, "stale"));
  ASSERT_OK(asvault_open(f.dir, &v));
  ASSERT_OK(asvault_set(v, "pw", "imap-password-1", &p));
  after = test_read_file(f.key, &len);
  ASSERT_TRUE(after != NULL && len == 32 && memcmp(after, key, 32) == 0);
  free(after);
  ASSERT_TRUE(!os_file_exists(tmp));
  ASSERT_OK(asvault_get(v, "pw", "imap", &value));
  ASSERT_EQ_STR(value, "imap-password-1");
  asvault_free_secret(value);
  asvault_close(v);
  fx_done(&f);
}

#if !defined(_WIN32)
TEST(passphrase_mode) {
  fixture f;
  asvault *v = NULL, *w = NULL;
  asvault_policy p = pol(IMAP, 1, NULL, 0);
  char *value = NULL, *text;
  ASSERT_TRUE(fx_init(&f));
  ASSERT_TRUE(setenv(ASVAULT_PASSPHRASE_ENV, "correct horse battery", 1) == 0);
  ASSERT_OK(asvault_open(f.dir, &v));
  ASSERT_OK(asvault_set(v, "pw", "imap-password-1", &p));
  ASSERT_OK(asvault_set(v, "pw2", "imap-password-2", &p)); /* cached key */
  ASSERT_TRUE(!os_file_exists(f.key));
  text = test_read_file(f.file, NULL);
  ASSERT_TRUE(text != NULL);
  ASSERT_TRUE(strstr(text, "mode: \"passphrase\"") != NULL);
  ASSERT_TRUE(strstr(text, "kdf: \"pbkdf2-hmac-sha256\"") != NULL);
  ASSERT_TRUE(strstr(text, "iterations: 600000") != NULL);
  ASSERT_TRUE(strstr(text, "salt: b\"") != NULL);
  free(text);

  /* a fresh handle derives the key again */
  ASSERT_OK(asvault_open(f.dir, &w));
  ASSERT_OK(asvault_get(w, "pw", "imap", &value));
  ASSERT_EQ_STR(value, "imap-password-1");
  asvault_free_secret(value);
  asvault_close(w);

  ASSERT_TRUE(unsetenv(ASVAULT_PASSPHRASE_ENV) == 0);
  ASSERT_OK(asvault_open(f.dir, &w));
  ASSERT_ERR(asvault_get(w, "pw", "imap", &value), ASVAULT_ERR_LOCKED);
  ASSERT_TRUE(strstr(asvault_last_error(w), ASVAULT_PASSPHRASE_ENV) != NULL);
  ASSERT_ERR(asvault_redact(w, "text", &value), ASVAULT_ERR_LOCKED);
  ASSERT_TRUE(setenv(ASVAULT_PASSPHRASE_ENV, "wrong horse", 1) == 0);
  ASSERT_ERR(asvault_get(w, "pw", "imap", &value), ASVAULT_ERR_LOCKED);
  ASSERT_TRUE(strstr(asvault_last_error(w), "wrong vault passphrase") != NULL);
  ASSERT_ERR(asvault_set(w, "pw3", "imap-password-3", &p), ASVAULT_ERR_LOCKED);
  ASSERT_TRUE(value == NULL);
  asvault_close(w);

  /* iterations below the floor are refused, not used */
  ASSERT_TRUE(edit_file(f.file, "iterations: 600000", "iterations: 1000"));
  ASSERT_TRUE(setenv(ASVAULT_PASSPHRASE_ENV, "correct horse battery", 1) == 0);
  ASSERT_OK(asvault_open(f.dir, &w));
  ASSERT_ERR(asvault_get(w, "pw", "imap", &value), ASVAULT_ERR_PARSE);
  asvault_close(w);
  asvault_close(v);
  fx_done(&f);
}
#endif

#if !defined(ASTERISM_NO_THREADS)
typedef struct {
  asvault *v;
  int id;
  int failures;
} worker_arg;

static void *worker(void *p) {
  worker_arg *a = (worker_arg *)p;
  asvault_policy pl = pol(IMAP, 1, NULL, 0);
  char name[32], value[64], *got;
  int i;
  for (i = 0; i < 8; i++) {
    snprintf(name, sizeof name, "w%d-%d", a->id, i);
    snprintf(value, sizeof value, "value-of-%s", name);
    if (asvault_set(a->v, name, value, &pl) != ASVAULT_OK) {
      a->failures++;
      continue;
    }
    got = NULL;
    if (asvault_get(a->v, name, "imap", &got) != ASVAULT_OK ||
        strcmp(got, value) != 0)
      a->failures++;
    asvault_free_secret(got);
  }
  return NULL;
}

TEST(concurrent_writers_lose_nothing) {
  fixture f;
  asvault *shared = NULL, *own[2] = {NULL, NULL};
  worker_arg args[4];
  os_thread th[4];
  asvault_entry_info *list = NULL;
  size_t n = 0;
  int i;
  ASSERT_TRUE(fx_init(&f));
  ASSERT_OK(asvault_open(f.dir, &shared));
  ASSERT_OK(asvault_open(f.dir, &own[0]));
  ASSERT_OK(asvault_open(f.dir, &own[1]));
  for (i = 0; i < 4; i++) {
    /* two threads share a handle; two have a handle each */
    args[i].v = i < 2 ? shared : own[i - 2];
    args[i].id = i;
    args[i].failures = 0;
    ASSERT_EQ_INT(os_thread_start(&th[i], worker, &args[i]), CORE_OK);
  }
  for (i = 0; i < 4; i++) os_thread_join(&th[i]);
  for (i = 0; i < 4; i++) ASSERT_EQ_INT(args[i].failures, 0);
  ASSERT_OK(asvault_list(shared, &list, &n));
  ASSERT_EQ_INT(n, 32);
  asvault_list_free(list, n);
  asvault_close(own[0]);
  asvault_close(own[1]);
  asvault_close(shared);
  fx_done(&f);
}
#endif

TEST(error_names) {
  ASSERT_EQ_STR(asvault_err_name(ASVAULT_OK), "ASVAULT_OK");
  ASSERT_EQ_STR(asvault_err_name(ASVAULT_ERR_DENIED), "ASVAULT_ERR_DENIED");
  ASSERT_EQ_STR(asvault_err_name(ASVAULT_ERR_LOCKED), "ASVAULT_ERR_LOCKED");
  ASSERT_EQ_STR(asvault_err_name((asvault_err)99), "ASVAULT_ERR_UNKNOWN");
  ASSERT_EQ_STR(asvault_last_error(NULL), "");
  asvault_close(NULL);
  asvault_free_secret(NULL);
}

TEST_LIST = {
    TEST_ENTRY(open_reads_and_creates_nothing),
    TEST_ENTRY(round_trip_set_get_list_delete),
    TEST_ENTRY(policy_is_normalized),
    TEST_ENTRY(invalid_input_is_rejected_before_any_write),
#if !defined(_WIN32)
    TEST_ENTRY(files_are_owner_only),
#endif
    TEST_ENTRY(aad_binds_the_name),
    TEST_ENTRY(aad_binds_the_policy),
    TEST_ENTRY(tampered_ciphertext_fails_authentication),
    TEST_ENTRY(corrupt_file_is_reported_and_never_overwritten),
    TEST_ENTRY(key_file_missing_or_foreign_locks_the_vault),
    TEST_ENTRY(leftover_key_file_and_temp_file_are_handled),
#if !defined(_WIN32)
    TEST_ENTRY(passphrase_mode),
#endif
#if !defined(ASTERISM_NO_THREADS)
    TEST_ENTRY(concurrent_writers_lose_nothing),
#endif
    TEST_ENTRY(error_names),
};

RUN_ALL_TESTS()
