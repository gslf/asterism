/*
 * test_policy.c — who receives a value: asvault_get for host consumers,
 * asvault_resolve at the tool boundary (tool and domain checks, several,
 * repeated, unknown and malformed placeholders), asvault_placeholders.
 */

#include "asvault_test.h"

#include "asvault_internal.h"

typedef struct {
  char base[256];
  char dir[320];
  asvault *v;
} fixture;

static asvault_policy pol(const char *const *tools, size_t nt,
                          const char *const *domains, size_t nd) {
  asvault_policy p;
  p.tools = tools;
  p.n_tools = nt;
  p.domains = domains;
  p.n_domains = nd;
  return p;
}

static const char *const T_IMAP[] = {"imap"};
static const char *const T_NET[] = {"http", "net"};
static const char *const T_TG[] = {"telegram"};
static const char *const D_IMAP[] = {"imap.example.com"};
static const char *const D_API[] = {"*.example.org"};

/* imap-pw: imap @ imap.example.com; api-key: http,net @ *.example.org;
 * bot: telegram, any domain. */
static int fx_init(fixture *f) {
  asvault_policy pi = pol(T_IMAP, 1, D_IMAP, 1);
  asvault_policy pn = pol(T_NET, 2, D_API, 1);
  asvault_policy pt = pol(T_TG, 1, NULL, 0);
#if !defined(_WIN32)
  unsetenv(ASVAULT_PASSPHRASE_ENV);
#endif
  if (!test_tmpdir(f->base)) return 0;
  snprintf(f->dir, sizeof f->dir, "%s/vault", f->base);
  if (asvault_open(f->dir, &f->v) != ASVAULT_OK) return 0;
  return asvault_set(f->v, "imap-pw", "imap-secret-1", &pi) == ASVAULT_OK &&
         asvault_set(f->v, "api-key", "sk-live-0123456789", &pn) ==
             ASVAULT_OK &&
         asvault_set(f->v, "bot", "123:telegram-token", &pt) == ASVAULT_OK;
}

static void fx_done(fixture *f) {
  asvault_close(f->v);
  test_rmtree(f->base);
}

TEST(get_only_for_listed_consumers) {
  fixture f;
  char *value = NULL;
  ASSERT_TRUE(fx_init(&f));
  ASSERT_OK(asvault_get(f.v, "bot", "telegram", &value));
  ASSERT_EQ_STR(value, "123:telegram-token");
  asvault_free_secret(value);
  value = NULL;
  ASSERT_ERR(asvault_get(f.v, "bot", "imap", &value), ASVAULT_ERR_DENIED);
  ASSERT_TRUE(value == NULL);
  ASSERT_TRUE(strstr(asvault_last_error(f.v), "'bot'") != NULL);
  ASSERT_TRUE(strstr(asvault_last_error(f.v), "'imap'") != NULL);
  ASSERT_ERR(asvault_get(f.v, "api-key", "telegram", &value),
             ASVAULT_ERR_DENIED);
  ASSERT_OK(asvault_get(f.v, "api-key", "http", &value));
  asvault_free_secret(value);
  ASSERT_ERR(asvault_get(f.v, "bot", "Telegram", &value),
             ASVAULT_ERR_INVALID);
  ASSERT_ERR(asvault_get(f.v, "bot", NULL, &value), ASVAULT_ERR_INVALID);
  ASSERT_ERR(asvault_get(f.v, "bot", "telegram", NULL), ASVAULT_ERR_INVALID);
  ASSERT_ERR(asvault_get(f.v, "nope", "telegram", &value),
             ASVAULT_ERR_NOT_FOUND);
  fx_done(&f);
}

TEST(resolve_checks_tool_and_domain) {
  fixture f;
  char *out = NULL;
  const char *text = "LOGIN user {{secret:imap-pw}}";
  ASSERT_TRUE(fx_init(&f));
  ASSERT_OK(asvault_resolve(f.v, text, "imap", "imap.example.com", &out));
  ASSERT_EQ_STR(out, "LOGIN user imap-secret-1");
  asvault_free_secret(out);
  /* host names compare case-insensitively, trailing dot ignored */
  ASSERT_OK(asvault_resolve(f.v, text, "imap", "IMAP.Example.com.", &out));
  asvault_free_secret(out);

  ASSERT_ERR(asvault_resolve(f.v, text, "imap", "evil.example.net", &out),
             ASVAULT_ERR_DENIED);
  ASSERT_TRUE(out == NULL);
  ASSERT_TRUE(strstr(asvault_last_error(f.v), "imap-pw") != NULL);
  ASSERT_TRUE(strstr(asvault_last_error(f.v), "evil.example.net") != NULL);
  ASSERT_ERR(asvault_resolve(f.v, text, "imap", NULL, &out),
             ASVAULT_ERR_DENIED);
  ASSERT_TRUE(strstr(asvault_last_error(f.v), "names none") != NULL);
  ASSERT_ERR(asvault_resolve(f.v, text, "net", "imap.example.com", &out),
             ASVAULT_ERR_DENIED);
  ASSERT_TRUE(strstr(asvault_last_error(f.v), "tool 'net'") != NULL);
  ASSERT_ERR(asvault_resolve(f.v, text, "imap", "sub.imap.example.com", &out),
             ASVAULT_ERR_DENIED);

  /* wildcard: any subdomain, not the apex nor a look-alike */
  ASSERT_OK(asvault_resolve(f.v, "Bearer {{secret:api-key}}", "net",
                            "api.example.org", &out));
  ASSERT_EQ_STR(out, "Bearer sk-live-0123456789");
  asvault_free_secret(out);
  ASSERT_OK(asvault_resolve(f.v, "{{secret:api-key}}", "http",
                            "a.b.example.org", &out));
  asvault_free_secret(out);
  ASSERT_ERR(asvault_resolve(f.v, "{{secret:api-key}}", "net", "example.org",
                             &out),
             ASVAULT_ERR_DENIED);
  ASSERT_ERR(asvault_resolve(f.v, "{{secret:api-key}}", "net",
                             "badexample.org", &out),
             ASVAULT_ERR_DENIED);

  /* no domain list: any target, or none */
  ASSERT_OK(asvault_resolve(f.v, "{{secret:bot}}", "telegram", NULL, &out));
  asvault_free_secret(out);
  ASSERT_OK(asvault_resolve(f.v, "{{secret:bot}}", "telegram",
                            "api.telegram.org", &out));
  ASSERT_EQ_STR(out, "123:telegram-token");
  asvault_free_secret(out);

  /* bad arguments */
  ASSERT_ERR(asvault_resolve(f.v, text, "imap", "exa mple.com", &out),
             ASVAULT_ERR_INVALID);
  ASSERT_ERR(asvault_resolve(f.v, text, "imap", "*.example.com", &out),
             ASVAULT_ERR_INVALID);
  ASSERT_ERR(asvault_resolve(f.v, text, "IMAP", NULL, &out),
             ASVAULT_ERR_INVALID);
  ASSERT_ERR(asvault_resolve(f.v, text, NULL, NULL, &out),
             ASVAULT_ERR_INVALID);
  ASSERT_ERR(asvault_resolve(f.v, NULL, "imap", NULL, &out),
             ASVAULT_ERR_INVALID);
  ASSERT_ERR(asvault_resolve(NULL, text, "imap", NULL, &out),
             ASVAULT_ERR_INVALID);
  fx_done(&f);
}

TEST(resolve_several_and_repeated) {
  static const char *const both[] = {"http", "net"};
  fixture f;
  asvault_policy p = pol(both, 2, NULL, 0);
  char *out = NULL;
  ASSERT_TRUE(fx_init(&f));
  ASSERT_OK(asvault_set(f.v, "user", "alice@example.org", &p));
  ASSERT_OK(asvault_set(f.v, "pass", "hunter2-hunter2", &p));
  ASSERT_OK(asvault_resolve(
      f.v, "{{secret:user}}:{{secret:pass}} again {{secret:user}}{{secret:pass}}",
      "net", NULL, &out));
  ASSERT_EQ_STR(out, "alice@example.org:hunter2-hunter2 again "
                     "alice@example.orghunter2-hunter2");
  asvault_free_secret(out);
  /* placeholders that merely look alike stay literal */
  ASSERT_OK(asvault_resolve(f.v, "{secret:user} {{ secret }} {{secret}}",
                            "net", NULL, &out));
  ASSERT_EQ_STR(out, "{secret:user} {{ secret }} {{secret}}");
  asvault_free_secret(out);
  /* a value is never expanded again */
  ASSERT_OK(asvault_set(f.v, "meta", "{{secret:pass}}-literal", &p));
  ASSERT_OK(asvault_resolve(f.v, "[{{secret:meta}}]", "net", NULL, &out));
  ASSERT_EQ_STR(out, "[{{secret:pass}}-literal]");
  asvault_free_secret(out);
  fx_done(&f);
}

TEST(resolve_checks_every_placeholder_first) {
  fixture f;
  char *out = NULL;
  ASSERT_TRUE(fx_init(&f));
  /* the first is allowed, the second not: nothing is substituted */
  ASSERT_ERR(asvault_resolve(f.v, "{{secret:bot}} {{secret:imap-pw}}",
                             "telegram", NULL, &out),
             ASVAULT_ERR_DENIED);
  ASSERT_TRUE(out == NULL);
  ASSERT_TRUE(strstr(asvault_last_error(f.v), "imap-pw") != NULL);
  ASSERT_ERR(asvault_resolve(f.v, "{{secret:bot}} {{secret:nope}}",
                             "telegram", NULL, &out),
             ASVAULT_ERR_NOT_FOUND);
  ASSERT_TRUE(out == NULL);
  ASSERT_TRUE(strstr(asvault_last_error(f.v), "'nope'") != NULL);
  fx_done(&f);
}

TEST(resolve_rejects_malformed_placeholders) {
  static const char *const bad[] = {
      "{{secret:Upper}}", "{{secret:}}",      "x {{secret:abc",
      "{{secret:-x}}",    "{{secret:abc }}",  "{{secret:a_b}}",
      "{{secret:abc}",    "ok {{secret:bot}} then {{secret:",
  };
  fixture f;
  char *out = NULL, longname[100];
  size_t i;
  ASSERT_TRUE(fx_init(&f));
  for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    ASSERT_ERR(asvault_resolve(f.v, bad[i], "telegram", NULL, &out),
               ASVAULT_ERR_INVALID);
    ASSERT_TRUE(out == NULL);
    ASSERT_TRUE(strstr(asvault_last_error(f.v), "malformed") != NULL);
  }
  snprintf(longname, sizeof longname, "{{secret:%065d}}", 0);
  ASSERT_ERR(asvault_resolve(f.v, longname, "telegram", NULL, &out),
             ASVAULT_ERR_INVALID);
  fx_done(&f);
}

TEST(resolve_without_placeholders_needs_no_vault) {
  char base[256], dir[300], *out = NULL;
  asvault *v = NULL;
  ASSERT_TRUE(test_tmpdir(base));
  snprintf(dir, sizeof dir, "%s/none", base);
  ASSERT_OK(asvault_open(dir, &v));
  ASSERT_OK(asvault_resolve(v, "plain text {not a placeholder}", "net", NULL,
                            &out));
  ASSERT_EQ_STR(out, "plain text {not a placeholder}");
  asvault_free_secret(out);
  ASSERT_OK(asvault_resolve(v, "", "net", NULL, &out));
  ASSERT_EQ_STR(out, "");
  asvault_free_secret(out);
  ASSERT_ERR(asvault_resolve(v, "{{secret:x}}", "net", NULL, &out),
             ASVAULT_ERR_NOT_FOUND);
  ASSERT_TRUE(!os_file_exists(dir));
  asvault_close(v);
  test_rmtree(base);
}

TEST(placeholders_listed_once_in_order) {
  char **names = NULL;
  size_t n = 9;
  ASSERT_OK(asvault_placeholders(
      "x {{secret:b}} {{secret:a}} {{secret:b}} {{{secret:c-1}}}", &names,
      &n));
  ASSERT_EQ_INT(n, 3);
  ASSERT_EQ_STR(names[0], "b");
  ASSERT_EQ_STR(names[1], "a");
  ASSERT_EQ_STR(names[2], "c-1");
  asvault_free_names(names, n);
  ASSERT_OK(asvault_placeholders("nothing here", &names, &n));
  ASSERT_EQ_INT(n, 0);
  ASSERT_TRUE(names == NULL);
  asvault_free_names(names, n);
  ASSERT_ERR(asvault_placeholders("{{secret:Bad}}", &names, &n),
             ASVAULT_ERR_INVALID);
  ASSERT_TRUE(names == NULL && n == 0);
  ASSERT_ERR(asvault_placeholders(NULL, &names, &n), ASVAULT_ERR_INVALID);
  ASSERT_ERR(asvault_placeholders("x", NULL, &n), ASVAULT_ERR_INVALID);
}

TEST(domain_helpers) {
  char out[ASVAULT_DOMAIN_MAX + 1], longer[300];
  ASSERT_TRUE(asvault_domain_normalize("Mail.Example.COM.", false, out));
  ASSERT_EQ_STR(out, "mail.example.com");
  ASSERT_TRUE(asvault_domain_normalize("10.0.0.1", false, out));
  ASSERT_TRUE(asvault_domain_normalize("localhost", false, out));
  ASSERT_TRUE(asvault_domain_normalize("xn--bcher-kva.example", false, out));
  ASSERT_TRUE(!asvault_domain_normalize("*.example.com", false, out));
  ASSERT_TRUE(asvault_domain_normalize("*.Example.com", true, out));
  ASSERT_EQ_STR(out, "*.example.com");
  ASSERT_TRUE(!asvault_domain_normalize("*.", true, out));
  ASSERT_TRUE(!asvault_domain_normalize("a..b", false, out));
  ASSERT_TRUE(!asvault_domain_normalize(".a", false, out));
  ASSERT_TRUE(!asvault_domain_normalize("a-.b", false, out));
  ASSERT_TRUE(!asvault_domain_normalize("a.-b", false, out));
  ASSERT_TRUE(!asvault_domain_normalize("a_b.com", false, out));
  ASSERT_TRUE(!asvault_domain_normalize("a.b:443", false, out));
  ASSERT_TRUE(!asvault_domain_normalize("", false, out));
  ASSERT_TRUE(!asvault_domain_normalize(NULL, false, out));
  memset(longer, 'a', 64);
  strcpy(longer + 64, ".com");
  ASSERT_TRUE(!asvault_domain_normalize(longer, false, out)); /* label > 63 */
  ASSERT_TRUE(asvault_domain_match("*.example.com", "a.example.com"));
  ASSERT_TRUE(!asvault_domain_match("*.example.com", "example.com"));
  ASSERT_TRUE(!asvault_domain_match("*.example.com", "aexample.com"));
  ASSERT_TRUE(asvault_domain_match("example.com", "example.com"));
  ASSERT_TRUE(!asvault_domain_match("example.com", "a.example.com"));
}

TEST_LIST = {
    TEST_ENTRY(get_only_for_listed_consumers),
    TEST_ENTRY(resolve_checks_tool_and_domain),
    TEST_ENTRY(resolve_several_and_repeated),
    TEST_ENTRY(resolve_checks_every_placeholder_first),
    TEST_ENTRY(resolve_rejects_malformed_placeholders),
    TEST_ENTRY(resolve_without_placeholders_needs_no_vault),
    TEST_ENTRY(placeholders_listed_once_in_order),
    TEST_ENTRY(domain_helpers),
};

RUN_ALL_TESTS()
