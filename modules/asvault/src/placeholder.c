/*
 * placeholder.c — {{secret:NAME}} surrogates: scanning, listing for
 * approval previews, and resolution into real values at the tool boundary.
 *
 * A placeholder is exactly "{{secret:" + slug + "}}". Anything else that
 * starts with "{{secret:" is malformed and rejected, so a typo never
 * reaches a tool as literal text. Resolution checks every referenced
 * secret against the policy before it decrypts or substitutes anything,
 * and builds the output in one exact-size allocation (no realloc copies of
 * plaintext).
 */

#include "asvault_internal.h"

#include <stdlib.h>
#include <string.h>

#define PH_OPEN  "{{secret:"
#define PH_CLOSE "}}"

static bool slug_char(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
}

asvault_err asvault_ph_scan(const char *text, asvault_ph **out, size_t *out_n,
                            size_t *bad_at) {
  const size_t open_len = sizeof PH_OPEN - 1;
  asvault_ph *v = NULL, *grown;
  size_t n = 0, cap = 0, pos = 0;
  const char *hit;

  *out = NULL;
  *out_n = 0;
  if (bad_at) *bad_at = 0;
  while ((hit = strstr(text + pos, PH_OPEN)) != NULL) {
    size_t start = (size_t)(hit - text), len = 0;
    const char *name = hit + open_len;
    while (len <= ASVAULT_NAME_MAX && slug_char(name[len])) len++;
    if (len == 0 || len > ASVAULT_NAME_MAX ||
        strncmp(name + len, PH_CLOSE, 2) != 0 || name[0] == '-') {
      free(v);
      if (bad_at) *bad_at = start;
      return ASVAULT_ERR_INVALID;
    }
    if (n == cap) {
      cap = cap ? cap * 2 : 4;
      grown = (asvault_ph *)realloc(v, cap * sizeof *v);
      if (!grown) {
        free(v);
        return ASVAULT_ERR_NOMEM;
      }
      v = grown;
    }
    v[n].start = start;
    v[n].end = start + open_len + len + 2;
    memcpy(v[n].name, name, len);
    v[n].name[len] = '\0';
    n++;
    pos = v[n - 1].end;
  }
  *out = v;
  *out_n = n;
  return ASVAULT_OK;
}

void asvault_free_names(char **names, size_t n) { asvault_strv_free(names, n); }

asvault_err asvault_placeholders(const char *text, char ***out_names,
                                 size_t *out_n) {
  asvault_ph *ph = NULL;
  char **names;
  size_t n = 0, i, j, k = 0;
  asvault_err e;

  if (out_names) *out_names = NULL;
  if (out_n) *out_n = 0;
  if (!text || !out_names || !out_n) return ASVAULT_ERR_INVALID;
  e = asvault_ph_scan(text, &ph, &n, NULL);
  if (e != ASVAULT_OK || n == 0) return e;
  names = (char **)calloc(n, sizeof *names);
  if (!names) {
    free(ph);
    return ASVAULT_ERR_NOMEM;
  }
  for (i = 0; i < n; i++) {
    for (j = 0; j < k; j++)
      if (strcmp(names[j], ph[i].name) == 0) break;
    if (j < k) continue;
    names[k] = core_strdup(ph[i].name);
    if (!names[k]) {
      asvault_strv_free(names, k);
      free(ph);
      return ASVAULT_ERR_NOMEM;
    }
    k++;
  }
  free(ph);
  *out_names = names;
  *out_n = k;
  return ASVAULT_OK;
}

/* ---- resolution --------------------------------------------------------- */

typedef struct {
  const asvault_rec *rec;
  char  *value;
  size_t len;
} rs_slot;

static void slots_free(rs_slot *s, size_t n) {
  size_t i;
  if (!s) return;
  for (i = 0; i < n; i++) asvault_free_secret(s[i].value);
  free(s);
}

static asvault_err resolve_locked(asvault *v, const char *text,
                                  const char *tool_id, const char *domain,
                                  char **out_text) {
  char host[ASVAULT_DOMAIN_MAX + 1];
  const char *hostp = NULL;
  asvault_doc d;
  asvault_ph *ph = NULL;
  rs_slot *slots = NULL;
  size_t n = 0, ns = 0, i, j, total, bad_at = 0, text_len;
  size_t *slot_of = NULL;
  char *out = NULL, *o;
  asvault_err e;

  if (!tool_id || !core_slug_valid(tool_id))
    return asvault_seterr(v, ASVAULT_ERR_INVALID,
                          "resolve: tool id must be a slug");
  if (domain) {
    if (!asvault_domain_normalize(domain, false, host))
      return asvault_seterr(v, ASVAULT_ERR_INVALID,
                            "resolve: \"%.253s\" is not a domain name",
                            domain);
    hostp = host;
  }
  e = asvault_ph_scan(text, &ph, &n, &bad_at);
  if (e == ASVAULT_ERR_INVALID)
    return asvault_seterr(v, e,
                          "resolve: malformed secret placeholder at byte %zu "
                          "(expected {{secret:NAME}} with a slug name)",
                          bad_at);
  if (e != ASVAULT_OK) return asvault_seterr(v, e, "out of memory");

  text_len = strlen(text);
  if (n == 0) {
    out = (char *)malloc(text_len + 1);
    if (!out) return asvault_seterr(v, ASVAULT_ERR_NOMEM, "out of memory");
    memcpy(out, text, text_len + 1);
    *out_text = out;
    return ASVAULT_OK;
  }

  e = asvault_doc_load(v, &d);
  if (e != ASVAULT_OK) {
    free(ph);
    return e;
  }
  slots = (rs_slot *)calloc(n, sizeof *slots);
  slot_of = (size_t *)calloc(n, sizeof *slot_of);
  if (!slots || !slot_of) {
    e = asvault_seterr(v, ASVAULT_ERR_NOMEM, "out of memory");
    goto done;
  }

  /* policy first, for every placeholder, before any decryption */
  for (i = 0; i < n; i++) {
    const asvault_rec *r = asvault_doc_find(&d, ph[i].name);
    if (!r) {
      e = asvault_seterr(v, ASVAULT_ERR_NOT_FOUND, "unknown secret '%s'",
                         ph[i].name);
      goto done;
    }
    if (!asvault_rec_allows_tool(r, tool_id)) {
      e = asvault_seterr(v, ASVAULT_ERR_DENIED,
                         "secret '%s' is not allowed for tool '%s'",
                         ph[i].name, tool_id);
      goto done;
    }
    if (!asvault_rec_allows_domain(r, hostp)) {
      e = hostp ? asvault_seterr(v, ASVAULT_ERR_DENIED,
                                 "secret '%s' is not allowed for domain '%s'",
                                 ph[i].name, hostp)
                : asvault_seterr(v, ASVAULT_ERR_DENIED,
                                 "secret '%s' is restricted to domains and "
                                 "the invocation names none",
                                 ph[i].name);
      goto done;
    }
    for (j = 0; j < ns; j++)
      if (slots[j].rec == r) break;
    if (j == ns) slots[ns++].rec = r;
    slot_of[i] = j;
  }

  e = asvault_key_unlock(v, &d);
  if (e != ASVAULT_OK) goto done;
  for (j = 0; j < ns; j++) {
    e = asvault_rec_open(v, slots[j].rec, &slots[j].value, &slots[j].len);
    if (e != ASVAULT_OK) goto done;
  }

  total = text_len;
  for (i = 0; i < n; i++) {
    size_t vlen = slots[slot_of[i]].len;
    total -= ph[i].end - ph[i].start;
    if (total > (size_t)-1 - vlen - 1) {
      e = asvault_seterr(v, ASVAULT_ERR_NOMEM, "resolve: output too large");
      goto done;
    }
    total += vlen;
  }
  out = (char *)malloc(total + 1);
  if (!out) {
    e = asvault_seterr(v, ASVAULT_ERR_NOMEM, "out of memory");
    goto done;
  }
  o = out;
  j = 0; /* read position in text */
  for (i = 0; i < n; i++) {
    const rs_slot *s = &slots[slot_of[i]];
    memcpy(o, text + j, ph[i].start - j);
    o += ph[i].start - j;
    memcpy(o, s->value, s->len);
    o += s->len;
    j = ph[i].end;
  }
  memcpy(o, text + j, text_len - j);
  o += text_len - j;
  *o = '\0';
  *out_text = out;
  e = ASVAULT_OK;

done:
  slots_free(slots, ns);
  free(slot_of);
  free(ph);
  asvault_doc_free(&d);
  return e;
}

asvault_err asvault_resolve(asvault *v, const char *text, const char *tool_id,
                            const char *domain, char **out_text) {
  asvault_err e;
  if (out_text) *out_text = NULL;
  if (!v || !text || !out_text) return ASVAULT_ERR_INVALID;
  os_mutex_lock(&v->mu);
  e = resolve_locked(v, text, tool_id, domain, out_text);
  os_mutex_unlock(&v->mu);
  return e;
}
