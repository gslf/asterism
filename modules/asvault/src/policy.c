/*
 * policy.c — who may receive a secret: tool ids (slugs) and, optionally,
 * the domains an invocation may target with it.
 *
 * Domains are host names of letters, digits and hyphens in dot-separated
 * labels (IPv4 literals fit), normalized to ASCII lowercase without a
 * trailing dot. A policy entry "*.example.com" matches any subdomain of
 * example.com, not example.com itself.
 */

#include "asvault_internal.h"

#include <stdlib.h>
#include <string.h>

static bool label_char(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
}

bool asvault_domain_normalize(const char *in, bool wildcard,
                              char out[ASVAULT_DOMAIN_MAX + 1]) {
  size_t n, i, label = 0;
  const char *host;
  char *o;

  if (!in) return false;
  n = strlen(in);
  if (n > 0 && in[n - 1] == '.') n--; /* absolute form */
  if (n == 0 || n > ASVAULT_DOMAIN_MAX) return false;
  host = in;
  o = out;
  if (wildcard && n > 2 && in[0] == '*' && in[1] == '.') {
    *o++ = '*';
    *o++ = '.';
    host += 2;
    n -= 2;
  }
  for (i = 0; i < n; i++) {
    char ch = host[i];
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    if (ch == '.') {
      /* labels are 1..63 chars and neither start nor end with '-' */
      if (label == 0 || o[-1] == '-') return false;
      label = 0;
    } else {
      if (!label_char(ch) || (label == 0 && ch == '-') || ++label > 63)
        return false;
    }
    *o++ = ch;
  }
  if (label == 0 || o[-1] == '-') return false;
  *o = '\0';
  return true;
}

bool asvault_domain_match(const char *pattern, const char *host) {
  size_t pn, hn;
  if (!pattern || !host) return false;
  if (pattern[0] == '*' && pattern[1] == '.') {
    pattern++; /* ".example.com": a strict suffix at a label boundary */
    pn = strlen(pattern);
    hn = strlen(host);
    return hn > pn && strcmp(host + hn - pn, pattern) == 0;
  }
  return strcmp(pattern, host) == 0;
}

/* ---- string vectors ----------------------------------------------------- */

void asvault_strv_free(char **v, size_t n) {
  size_t i;
  if (!v) return;
  for (i = 0; i < n; i++) free(v[i]);
  free(v);
}

char **asvault_strv_dup(char *const *src, size_t n) {
  char **v;
  size_t i;
  v = (char **)calloc(n ? n : 1, sizeof *v);
  if (!v) return NULL;
  for (i = 0; i < n; i++) {
    v[i] = core_strdup(src[i]);
    if (!v[i]) {
      asvault_strv_free(v, i);
      return NULL;
    }
  }
  return v;
}

static int strv_cmp(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Sort and drop duplicates in place. */
static size_t strv_unique(char **v, size_t n) {
  size_t i, o = 0;
  if (n == 0) return 0;
  qsort(v, n, sizeof *v, strv_cmp);
  for (i = 0; i < n; i++) {
    if (o > 0 && strcmp(v[o - 1], v[i]) == 0) {
      free(v[i]);
      continue;
    }
    v[o++] = v[i];
  }
  return o;
}

asvault_err asvault_policy_copy(asvault *v, const asvault_policy *p,
                                char ***tools, size_t *n_tools,
                                char ***domains, size_t *n_domains) {
  char **t = NULL, **d = NULL;
  char buf[ASVAULT_DOMAIN_MAX + 1];
  size_t i, nt = 0, nd = 0;

  *tools = NULL;
  *domains = NULL;
  *n_tools = 0;
  *n_domains = 0;
  if (!p || p->n_tools == 0 || !p->tools)
    return asvault_seterr(v, ASVAULT_ERR_INVALID,
                          "policy: at least one tool is required");
  if (p->n_domains > 0 && !p->domains)
    return asvault_seterr(v, ASVAULT_ERR_INVALID, "policy: domains missing");
  if (p->n_tools > 256 || p->n_domains > 256)
    return asvault_seterr(v, ASVAULT_ERR_INVALID,
                          "policy: at most 256 tools and 256 domains");

  t = (char **)calloc(p->n_tools, sizeof *t);
  d = (char **)calloc(p->n_domains ? p->n_domains : 1, sizeof *d);
  if (!t || !d) goto nomem;
  for (i = 0; i < p->n_tools; i++) {
    const char *id = p->tools[i];
    if (!id || !core_slug_valid(id)) {
      asvault_strv_free(t, nt);
      asvault_strv_free(d, nd);
      return asvault_seterr(v, ASVAULT_ERR_INVALID,
                            "policy: tool id \"%.64s\" is not a slug "
                            "[a-z0-9][a-z0-9-]{0,63}",
                            id ? id : "(null)");
    }
    t[nt] = core_strdup(id);
    if (!t[nt]) goto nomem;
    nt++;
  }
  for (i = 0; i < p->n_domains; i++) {
    const char *dom = p->domains[i];
    if (!asvault_domain_normalize(dom, true, buf)) {
      asvault_strv_free(t, nt);
      asvault_strv_free(d, nd);
      return asvault_seterr(v, ASVAULT_ERR_INVALID,
                            "policy: \"%.253s\" is not a domain name",
                            dom ? dom : "(null)");
    }
    d[nd] = core_strdup(buf);
    if (!d[nd]) goto nomem;
    nd++;
  }
  *n_tools = strv_unique(t, nt);
  *n_domains = strv_unique(d, nd);
  *tools = t;
  *domains = d;
  return ASVAULT_OK;

nomem:
  asvault_strv_free(t, nt);
  asvault_strv_free(d, nd);
  return asvault_seterr(v, ASVAULT_ERR_NOMEM, "policy: out of memory");
}
