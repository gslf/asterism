/*
 * store.c — vault.xcdn, the master key and entry sealing.
 *
 * vault.xcdn is one #asvault object:
 *
 *   #asvault {
 *     version: 1,
 *     created_at: t"2026-10-26T09:00:00Z",
 *     cipher: "chacha20-poly1305",
 *     key: { mode: "file", check: b"..." },
 *       // or { mode: "passphrase", kdf: "pbkdf2-hmac-sha256",
 *       //      salt: b"...", iterations: 600000, check: b"..." }
 *     entries: [
 *       { name: "imap-password", created_at: t"...", updated_at: t"...",
 *         policy: { tools: ["imap"], domains: ["imap.example.com"] },
 *         nonce: b"...", ciphertext: b"..." },
 *     ],
 *   }
 *
 * The parser (xCDN-C) is strict: unknown keys, wrong types and bad sizes are
 * ASVAULT_ERR_PARSE, and a vault that fails to parse is never rewritten. The
 * writer emits the text directly: every string in it is a validated slug,
 * domain or timestamp, so nothing needs escaping.
 *
 * Keys. The master key is 32 random bytes in vault.key, or PBKDF2 of the
 * passphrase. Two values derive from it with HMAC-SHA256: the key-check
 * value stored in the header (a wrong key or passphrase is reported as
 * such instead of as a corrupt entry) and the entry key. Each entry is
 * ChaCha20-Poly1305 under the entry key with a random 96-bit nonce and
 * ciphertext || tag stored together; the AAD is
 *   "asvault/v1/entry" 0x00 || lp(name) || u32(n_tools) || lp(tool)... ||
 *   u32(n_domains) || lp(domain)...
 * where lp() is a little-endian u32 length followed by the bytes.
 */

#include "asvault_internal.h"

#include <stdlib.h>
#include <string.h>

#include "xcdn.h"

#define CHECK_LABEL "asvault/v1/key-check"
#define ENTRY_LABEL "asvault/v1/entry-key"
#define AAD_LABEL   "asvault/v1/entry"
#define KDF_NAME    "pbkdf2-hmac-sha256"
#define CIPHER_NAME "chacha20-poly1305"

/* ---- records ------------------------------------------------------------ */

void asvault_rec_free(asvault_rec *r) {
  if (!r) return;
  asvault_strv_free(r->tools, r->n_tools);
  asvault_strv_free(r->domains, r->n_domains);
  free(r->sealed);
  memset(r, 0, sizeof *r);
}

void asvault_doc_free(asvault_doc *d) {
  size_t i;
  if (!d) return;
  for (i = 0; i < d->n; i++) asvault_rec_free(&d->recs[i]);
  free(d->recs);
  memset(d, 0, sizeof *d);
}

asvault_rec *asvault_doc_find(const asvault_doc *d, const char *name) {
  size_t i;
  for (i = 0; i < d->n; i++)
    if (strcmp(d->recs[i].name, name) == 0) return &d->recs[i];
  return NULL;
}

asvault_err asvault_doc_append(asvault *v, asvault_doc *d, const char *name,
                               asvault_rec **out) {
  asvault_rec *grown, *r;
  if (strlen(name) > ASVAULT_NAME_MAX)
    return asvault_seterr(v, ASVAULT_ERR_INVALID, "secret name too long");
  grown = (asvault_rec *)realloc(d->recs, (d->n + 1) * sizeof *grown);
  if (!grown) return asvault_seterr(v, ASVAULT_ERR_NOMEM, "out of memory");
  d->recs = grown;
  r = &d->recs[d->n++];
  memset(r, 0, sizeof *r);
  strcpy(r->name, name);
  *out = r;
  return ASVAULT_OK;
}

void asvault_doc_remove(asvault_doc *d, asvault_rec *r) {
  size_t i = (size_t)(r - d->recs);
  asvault_rec_free(r);
  memmove(&d->recs[i], &d->recs[i + 1], (d->n - i - 1) * sizeof *r);
  d->n--;
}

bool asvault_rec_allows_tool(const asvault_rec *r, const char *tool) {
  size_t i;
  if (!tool) return false;
  for (i = 0; i < r->n_tools; i++)
    if (strcmp(r->tools[i], tool) == 0) return true;
  return false;
}

bool asvault_rec_allows_domain(const asvault_rec *r, const char *host) {
  size_t i;
  if (r->n_domains == 0) return true;
  if (!host) return false;
  for (i = 0; i < r->n_domains; i++)
    if (asvault_domain_match(r->domains[i], host)) return true;
  return false;
}

/* ---- parsing ------------------------------------------------------------ */

static asvault_err perr(asvault *v, const char *what, const char *detail) {
  return asvault_seterr(v, ASVAULT_ERR_PARSE, "%s is corrupt: %s %s",
                        v->file_path, what, detail);
}

static const char *str_of(const xcdn_value_t *val) {
  return val && val->type == XCDN_VAL_STRING ? val->data.string : NULL;
}

/* A t"..." (or plain string) RFC 3339 timestamp into out. */
static bool time_of(const xcdn_value_t *val, char out[CORE_RFC3339_SIZE]) {
  core_time t;
  const char *s;
  if (!val ||
      (val->type != XCDN_VAL_DATETIME && val->type != XCDN_VAL_STRING))
    return false;
  s = val->data.string;
  if (!s || !core_time_parse_rfc3339(s, &t)) return false;
  core_time_format_rfc3339(t, out);
  return true;
}

static bool bytes_of(const xcdn_value_t *val, size_t min, size_t max,
                     const uint8_t **data, size_t *len) {
  if (!val || val->type != XCDN_VAL_BYTES) return false;
  if (val->data.bytes.len < min || val->data.bytes.len > max) return false;
  *data = val->data.bytes.data;
  *len = val->data.bytes.len;
  return true;
}

/* A string array into a fresh vector; each element passes check (which may
 * rewrite it into norm when non-NULL). */
static asvault_err strv_of(asvault *v, const xcdn_value_t *val,
                           const char *what, bool tools, char ***out,
                           size_t *out_n) {
  size_t i, n;
  char **vec;
  char norm[ASVAULT_DOMAIN_MAX + 1];

  *out = NULL;
  *out_n = 0;
  if (!val || val->type != XCDN_VAL_ARRAY)
    return perr(v, what, "must be an array of strings");
  n = xcdn_array_len(val);
  if (n > 256) return perr(v, what, "has more than 256 items");
  vec = (char **)calloc(n ? n : 1, sizeof *vec);
  if (!vec) return asvault_seterr(v, ASVAULT_ERR_NOMEM, "out of memory");
  for (i = 0; i < n; i++) {
    const xcdn_node_t *item = xcdn_array_get(val, i);
    const char *s = item ? str_of(item->value) : NULL;
    bool ok = s && (tools ? core_slug_valid(s)
                          : asvault_domain_normalize(s, true, norm) &&
                                strcmp(norm, s) == 0);
    if (!ok) {
      asvault_strv_free(vec, i);
      return perr(v, what, tools ? "holds an invalid tool id"
                                 : "holds an invalid domain");
    }
    vec[i] = core_strdup(s);
    if (!vec[i]) {
      asvault_strv_free(vec, i);
      return asvault_seterr(v, ASVAULT_ERR_NOMEM, "out of memory");
    }
  }
  *out = vec;
  *out_n = n;
  return ASVAULT_OK;
}

static asvault_err parse_policy(asvault *v, const xcdn_value_t *val,
                                asvault_rec *r) {
  size_t i;
  bool seen_tools = false;
  asvault_err e;
  if (!val || val->type != XCDN_VAL_OBJECT)
    return perr(v, "an entry policy", "must be an object");
  for (i = 0; i < xcdn_object_len(val); i++) {
    const char *k = xcdn_object_key_at(val, i);
    const xcdn_node_t *n = xcdn_object_node_at(val, i);
    const xcdn_value_t *x = n ? n->value : NULL;
    if (k && strcmp(k, "tools") == 0) {
      e = strv_of(v, x, "policy.tools", true, &r->tools, &r->n_tools);
      if (e != ASVAULT_OK) return e;
      if (r->n_tools == 0) return perr(v, "policy.tools", "is empty");
      seen_tools = true;
    } else if (k && strcmp(k, "domains") == 0) {
      e = strv_of(v, x, "policy.domains", false, &r->domains, &r->n_domains);
      if (e != ASVAULT_OK) return e;
    } else {
      return perr(v, "an entry policy", "has an unknown key");
    }
  }
  if (!seen_tools) return perr(v, "an entry policy", "lacks tools");
  return ASVAULT_OK;
}

static asvault_err parse_entry(asvault *v, const xcdn_value_t *val,
                               asvault_doc *d) {
  enum { F_NAME = 1, F_CREATED = 2, F_UPDATED = 4, F_POLICY = 8, F_NONCE = 16,
         F_CT = 32, F_ALL = 63 };
  const xcdn_node_t *policy = NULL;
  const uint8_t *data;
  const char *name = NULL;
  asvault_rec *r;
  size_t i, len;
  unsigned seen = 0;
  asvault_err e;

  if (!val || val->type != XCDN_VAL_OBJECT)
    return perr(v, "an entry", "must be an object");
  for (i = 0; i < xcdn_object_len(val); i++) {
    const char *k = xcdn_object_key_at(val, i);
    if (k && strcmp(k, "name") == 0) {
      const xcdn_node_t *n = xcdn_object_node_at(val, i);
      name = n ? str_of(n->value) : NULL;
      if (!name || !core_slug_valid(name))
        return perr(v, "an entry name", "is not a slug");
    }
  }
  if (!name) return perr(v, "an entry", "lacks a name");
  if (asvault_doc_find(d, name))
    return asvault_seterr(v, ASVAULT_ERR_PARSE,
                          "%s is corrupt: secret '%s' appears twice",
                          v->file_path, name);
  e = asvault_doc_append(v, d, name, &r);
  if (e != ASVAULT_OK) return e;

  for (i = 0; i < xcdn_object_len(val); i++) {
    const char *k = xcdn_object_key_at(val, i);
    const xcdn_node_t *n = xcdn_object_node_at(val, i);
    const xcdn_value_t *x = n ? n->value : NULL;
    unsigned bit;
    if (!k) return perr(v, "an entry", "has an empty key");
    if (strcmp(k, "name") == 0) {
      bit = F_NAME;
    } else if (strcmp(k, "created_at") == 0) {
      bit = F_CREATED;
      if (!time_of(x, r->created_at))
        return perr(v, "an entry created_at", "is not an RFC 3339 time");
    } else if (strcmp(k, "updated_at") == 0) {
      bit = F_UPDATED;
      if (!time_of(x, r->updated_at))
        return perr(v, "an entry updated_at", "is not an RFC 3339 time");
    } else if (strcmp(k, "policy") == 0) {
      bit = F_POLICY;
      policy = n;
    } else if (strcmp(k, "nonce") == 0) {
      bit = F_NONCE;
      if (!bytes_of(x, ASVAULT_NONCE_SIZE, ASVAULT_NONCE_SIZE, &data, &len))
        return perr(v, "an entry nonce", "must be 12 bytes");
      memcpy(r->nonce, data, len);
    } else if (strcmp(k, "ciphertext") == 0) {
      bit = F_CT;
      if (!bytes_of(x, ASVAULT_TAG_SIZE + 1,
                    ASVAULT_VALUE_MAX + ASVAULT_TAG_SIZE, &data, &len))
        return perr(v, "an entry ciphertext", "has an impossible length");
      r->sealed = (uint8_t *)malloc(len);
      if (!r->sealed)
        return asvault_seterr(v, ASVAULT_ERR_NOMEM, "out of memory");
      memcpy(r->sealed, data, len);
      r->sealed_len = len;
    } else {
      return asvault_seterr(v, ASVAULT_ERR_PARSE,
                            "%s is corrupt: secret '%s' has an unknown key",
                            v->file_path, name);
    }
    if (seen & bit)
      return asvault_seterr(v, ASVAULT_ERR_PARSE,
                            "%s is corrupt: secret '%s' repeats a key",
                            v->file_path, name);
    seen |= bit;
  }
  if (seen != F_ALL)
    return asvault_seterr(v, ASVAULT_ERR_PARSE,
                          "%s is corrupt: secret '%s' lacks a required key",
                          v->file_path, name);
  return parse_policy(v, policy ? policy->value : NULL, r);
}

static asvault_err parse_key(asvault *v, const xcdn_value_t *val,
                             asvault_doc *d) {
  const uint8_t *data;
  const char *mode = NULL, *kdf = NULL;
  size_t i, len;
  bool have_check = false, have_salt = false;
  int64_t iterations = -1;

  if (!val || val->type != XCDN_VAL_OBJECT)
    return perr(v, "the key header", "must be an object");
  for (i = 0; i < xcdn_object_len(val); i++) {
    const char *k = xcdn_object_key_at(val, i);
    const xcdn_node_t *n = xcdn_object_node_at(val, i);
    const xcdn_value_t *x = n ? n->value : NULL;
    if (!k) return perr(v, "the key header", "has an empty key");
    if (strcmp(k, "mode") == 0) {
      mode = str_of(x);
      if (!mode) return perr(v, "key.mode", "must be a string");
    } else if (strcmp(k, "check") == 0) {
      if (!bytes_of(x, 32, 32, &data, &len))
        return perr(v, "key.check", "must be 32 bytes");
      memcpy(d->check, data, 32);
      have_check = true;
    } else if (strcmp(k, "kdf") == 0) {
      kdf = str_of(x);
      if (!kdf) return perr(v, "key.kdf", "must be a string");
    } else if (strcmp(k, "salt") == 0) {
      if (!bytes_of(x, ASVAULT_SALT_SIZE, ASVAULT_SALT_SIZE, &data, &len))
        return perr(v, "key.salt", "must be 16 bytes");
      memcpy(d->salt, data, len);
      have_salt = true;
    } else if (strcmp(k, "iterations") == 0) {
      if (!x || x->type != XCDN_VAL_INT)
        return perr(v, "key.iterations", "must be an integer");
      iterations = x->data.integer;
    } else {
      return perr(v, "the key header", "has an unknown key");
    }
  }
  if (!mode || !have_check)
    return perr(v, "the key header", "needs mode and check");
  if (strcmp(mode, "file") == 0) {
    if (kdf || have_salt || iterations != -1)
      return perr(v, "the key header", "mixes key-file and passphrase fields");
    d->mode = ASVAULT_MODE_KEYFILE;
    return ASVAULT_OK;
  }
  if (strcmp(mode, "passphrase") != 0)
    return perr(v, "key.mode", "must be \"file\" or \"passphrase\"");
  if (!kdf || strcmp(kdf, KDF_NAME) != 0)
    return perr(v, "key.kdf", "must be \"" KDF_NAME "\"");
  if (!have_salt) return perr(v, "the key header", "lacks the salt");
  if (iterations < ASVAULT_KDF_ITERATIONS ||
      iterations > (int64_t)ASVAULT_KDF_MAX_ITERATIONS)
    return perr(v, "key.iterations", "is out of range");
  d->mode = ASVAULT_MODE_PASSPHRASE;
  d->iterations = (uint32_t)iterations;
  return ASVAULT_OK;
}

static asvault_err parse_root(asvault *v, const xcdn_document_t *doc,
                              asvault_doc *d) {
  enum { F_VERSION = 1, F_CREATED = 2, F_CIPHER = 4, F_KEY = 8, F_ENTRIES = 16,
         F_ALL = 31 };
  const xcdn_node_t *root;
  const xcdn_value_t *rv;
  unsigned seen = 0;
  size_t i, j;
  asvault_err e;

  if (doc->values_len != 1 || !(root = doc->values[0]) || !root->value ||
      root->value->type != XCDN_VAL_OBJECT ||
      xcdn_node_tag_count(root) != 1 || !xcdn_node_has_tag(root, "asvault"))
    return perr(v, "the document", "must be one #asvault object");
  rv = root->value;
  for (i = 0; i < xcdn_object_len(rv); i++) {
    const char *k = xcdn_object_key_at(rv, i);
    const xcdn_node_t *n = xcdn_object_node_at(rv, i);
    const xcdn_value_t *x = n ? n->value : NULL;
    unsigned bit;
    if (!k) return perr(v, "the header", "has an empty key");
    if (strcmp(k, "version") == 0) {
      bit = F_VERSION;
      if (!x || x->type != XCDN_VAL_INT || x->data.integer != 1)
        return asvault_seterr(v, ASVAULT_ERR_PARSE,
                              "%s: unsupported vault version", v->file_path);
    } else if (strcmp(k, "created_at") == 0) {
      bit = F_CREATED;
      if (!time_of(x, d->created_at))
        return perr(v, "created_at", "is not an RFC 3339 time");
    } else if (strcmp(k, "cipher") == 0) {
      const char *s = str_of(x);
      bit = F_CIPHER;
      if (!s || strcmp(s, CIPHER_NAME) != 0)
        return perr(v, "cipher", "must be \"" CIPHER_NAME "\"");
    } else if (strcmp(k, "key") == 0) {
      bit = F_KEY;
      e = parse_key(v, x, d);
      if (e != ASVAULT_OK) return e;
    } else if (strcmp(k, "entries") == 0) {
      bit = F_ENTRIES;
      if (!x || x->type != XCDN_VAL_ARRAY)
        return perr(v, "entries", "must be an array");
      for (j = 0; j < xcdn_array_len(x); j++) {
        const xcdn_node_t *item = xcdn_array_get(x, j);
        e = parse_entry(v, item ? item->value : NULL, d);
        if (e != ASVAULT_OK) return e;
      }
    } else {
      return perr(v, "the header", "has an unknown key");
    }
    if (seen & bit) return perr(v, "the header", "repeats a key");
    seen |= bit;
  }
  if (seen != F_ALL) return perr(v, "the header", "lacks a required key");
  return ASVAULT_OK;
}

asvault_err asvault_doc_load(asvault *v, asvault_doc *d) {
  xcdn_document_t *doc;
  xcdn_error_t xerr;
  char *text = NULL;
  size_t len = 0;
  asvault_err e;

  memset(d, 0, sizeof *d);
  e = os_read_file(v->file_path, &text, &len);
  if (e == ASVAULT_ERR_NOT_FOUND) return ASVAULT_OK;
  if (e != ASVAULT_OK)
    return asvault_seterr(v, e == ASVAULT_ERR_NOMEM ? e : ASVAULT_ERR_IO,
                          "cannot read %s", v->file_path);
  if (strlen(text) != len) {
    free(text);
    return perr(v, "the file", "contains NUL bytes");
  }
  doc = xcdn_parse(text, &xerr);
  free(text);
  if (!doc)
    return asvault_seterr(v, ASVAULT_ERR_PARSE,
                          "%s is corrupt: parse error at line %zu: %s",
                          v->file_path, xerr.span.line, xerr.message);
  e = parse_root(v, doc, d);
  xcdn_document_free(doc);
  if (e != ASVAULT_OK) {
    asvault_doc_free(d);
    return e;
  }
  d->exists = 1;
  return ASVAULT_OK;
}

/* ---- serialization ------------------------------------------------------ */

static asvault_err put_bytes(core_buf *b, const char *key, const uint8_t *data,
                             size_t len, const char *indent) {
  char *enc = core_base64_encode(data, len);
  core_err ce;
  if (!enc) return ASVAULT_ERR_NOMEM;
  ce = core_buf_printf(b, "%s%s: b\"%s\",\n", indent, key, enc);
  free(enc);
  return asvault_from_core(ce);
}

static asvault_err put_list(core_buf *b, char *const *items, size_t n) {
  size_t i;
  core_err ce = core_buf_appendc(b, '[');
  for (i = 0; ce == CORE_OK && i < n; i++)
    ce = core_buf_printf(b, "%s\"%s\"", i ? ", " : "", items[i]);
  if (ce == CORE_OK) ce = core_buf_appendc(b, ']');
  return asvault_from_core(ce);
}

static int rec_cmp(const void *a, const void *b) {
  return strcmp(((const asvault_rec *)a)->name, ((const asvault_rec *)b)->name);
}

asvault_err asvault_doc_save(asvault *v, asvault_doc *d) {
  core_buf b;
  core_err ce;
  asvault_err e;
  size_t i;

  if (d->n > 1) qsort(d->recs, d->n, sizeof *d->recs, rec_cmp);
  core_buf_init(&b);
  ce = core_buf_printf(&b,
                       "// asterism vault: values are encrypted and every entry\n"
                       "// is authenticated together with its name and policy;\n"
                       "// edit it with \"asterism vault\", not by hand.\n"
                       "#asvault {\n"
                       "  version: 1,\n"
                       "  created_at: t\"%s\",\n"
                       "  cipher: \"" CIPHER_NAME "\",\n"
                       "  key: {\n"
                       "    mode: \"%s\",\n",
                       d->created_at,
                       d->mode == ASVAULT_MODE_PASSPHRASE ? "passphrase"
                                                          : "file");
  e = asvault_from_core(ce);
  if (e == ASVAULT_OK && d->mode == ASVAULT_MODE_PASSPHRASE) {
    e = asvault_from_core(
        core_buf_appends(&b, "    kdf: \"" KDF_NAME "\",\n"));
    if (e == ASVAULT_OK)
      e = put_bytes(&b, "salt", d->salt, sizeof d->salt, "    ");
    if (e == ASVAULT_OK)
      e = asvault_from_core(core_buf_printf(&b, "    iterations: %lu,\n",
                                            (unsigned long)d->iterations));
  }
  if (e == ASVAULT_OK) e = put_bytes(&b, "check", d->check, 32, "    ");
  if (e == ASVAULT_OK)
    e = asvault_from_core(core_buf_appends(&b, "  },\n  entries: [\n"));
  for (i = 0; e == ASVAULT_OK && i < d->n; i++) {
    const asvault_rec *r = &d->recs[i];
    e = asvault_from_core(core_buf_printf(&b,
                                          "    {\n"
                                          "      name: \"%s\",\n"
                                          "      created_at: t\"%s\",\n"
                                          "      updated_at: t\"%s\",\n"
                                          "      policy: { tools: ",
                                          r->name, r->created_at,
                                          r->updated_at));
    if (e == ASVAULT_OK) e = put_list(&b, r->tools, r->n_tools);
    if (e == ASVAULT_OK)
      e = asvault_from_core(core_buf_appends(&b, ", domains: "));
    if (e == ASVAULT_OK) e = put_list(&b, r->domains, r->n_domains);
    if (e == ASVAULT_OK)
      e = asvault_from_core(core_buf_appends(&b, " },\n"));
    if (e == ASVAULT_OK)
      e = put_bytes(&b, "nonce", r->nonce, sizeof r->nonce, "      ");
    if (e == ASVAULT_OK)
      e = put_bytes(&b, "ciphertext", r->sealed, r->sealed_len, "      ");
    if (e == ASVAULT_OK)
      e = asvault_from_core(core_buf_appends(&b, "    },\n"));
  }
  if (e == ASVAULT_OK)
    e = asvault_from_core(core_buf_appends(&b, "  ],\n}\n"));
  if (e != ASVAULT_OK) {
    core_buf_free(&b);
    return asvault_seterr(v, e, "out of memory");
  }
  e = os_private_replace(v->file_path, b.data, b.len);
  core_buf_free(&b);
  if (e != ASVAULT_OK)
    return asvault_seterr(v, e, "cannot write %s", v->file_path);
  return ASVAULT_OK;
}

/* ---- keys --------------------------------------------------------------- */

/* check value and entry key from the master key; caches them on v. */
static void key_install(asvault *v, const uint8_t master[32]) {
  core_hmac_sha256(master, 32, CHECK_LABEL, sizeof CHECK_LABEL - 1, v->check);
  core_hmac_sha256(master, 32, ENTRY_LABEL, sizeof ENTRY_LABEL - 1,
                   v->entry_key);
  v->unlocked = 1;
}

static void key_forget(asvault *v) {
  asvault_wipe(v->check, sizeof v->check);
  asvault_wipe(v->entry_key, sizeof v->entry_key);
  v->unlocked = 0;
}

/* vault.key into master: LOCKED when missing or malformed. */
static asvault_err key_file_read(asvault *v, uint8_t master[32]) {
  size_t len = 0;
  asvault_err e = os_private_read(v->key_path, master, 32, &len);
  if (e == ASVAULT_ERR_NOT_FOUND)
    return asvault_seterr(v, ASVAULT_ERR_LOCKED,
                          "vault key file %s is missing", v->key_path);
  if (e == ASVAULT_OK && len != 32) e = ASVAULT_ERR_INVALID;
  if (e == ASVAULT_ERR_INVALID) {
    asvault_wipe(master, 32);
    return asvault_seterr(v, ASVAULT_ERR_LOCKED,
                          "vault key file %s is not a 32-byte key",
                          v->key_path);
  }
  if (e != ASVAULT_OK)
    return asvault_seterr(v, ASVAULT_ERR_IO, "cannot read %s", v->key_path);
  return ASVAULT_OK;
}

static const char *passphrase_env(void) {
  const char *p = getenv(ASVAULT_PASSPHRASE_ENV);
  return p && *p ? p : NULL;
}

asvault_err asvault_key_create(asvault *v, asvault_doc *d) {
  uint8_t master[32];
  const char *pass = passphrase_env();
  asvault_err e;

  key_forget(v);
  core_time_format_rfc3339((core_time)os_now_unix(), d->created_at);
  if (pass) {
    d->mode = ASVAULT_MODE_PASSPHRASE;
    d->iterations = ASVAULT_KDF_ITERATIONS;
    os_random_bytes(d->salt, sizeof d->salt);
    if (!asvault_pbkdf2_sha256(pass, strlen(pass), d->salt, sizeof d->salt,
                               d->iterations, master, sizeof master))
      return asvault_seterr(v, ASVAULT_ERR_CRYPTO, "key derivation failed");
  } else {
    d->mode = ASVAULT_MODE_KEYFILE;
    /* a key file left by an interrupted first write is reused, never
     * replaced: it may already protect entries elsewhere */
    if (os_file_exists(v->key_path)) {
      e = key_file_read(v, master);
    } else {
      os_random_bytes(master, sizeof master);
      e = os_private_create(v->key_path, master, sizeof master);
      if (e != ASVAULT_OK)
        e = asvault_seterr(v, e, "cannot create %s", v->key_path);
    }
    if (e != ASVAULT_OK) {
      asvault_wipe(master, sizeof master);
      return e;
    }
  }
  key_install(v, master);
  memcpy(d->check, v->check, sizeof d->check);
  asvault_wipe(master, sizeof master);
  return ASVAULT_OK;
}

asvault_err asvault_key_unlock(asvault *v, const asvault_doc *d) {
  uint8_t master[32];
  const char *pass;
  asvault_err e;

  if (v->unlocked && core_bytes_equal_ct(v->check, d->check, sizeof v->check))
    return ASVAULT_OK;
  key_forget(v);
  if (d->mode == ASVAULT_MODE_PASSPHRASE) {
    pass = passphrase_env();
    if (!pass)
      return asvault_seterr(v, ASVAULT_ERR_LOCKED,
                            "the vault is passphrase-protected: set "
                            ASVAULT_PASSPHRASE_ENV);
    if (!asvault_pbkdf2_sha256(pass, strlen(pass), d->salt, sizeof d->salt,
                               d->iterations, master, sizeof master))
      return asvault_seterr(v, ASVAULT_ERR_CRYPTO, "key derivation failed");
  } else {
    e = key_file_read(v, master);
    if (e != ASVAULT_OK) return e;
  }
  key_install(v, master);
  asvault_wipe(master, sizeof master);
  if (!core_bytes_equal_ct(v->check, d->check, sizeof v->check)) {
    key_forget(v);
    return d->mode == ASVAULT_MODE_PASSPHRASE
               ? asvault_seterr(v, ASVAULT_ERR_LOCKED,
                                "wrong vault passphrase (" ASVAULT_PASSPHRASE_ENV
                                ")")
               : asvault_seterr(v, ASVAULT_ERR_LOCKED,
                                "vault key file %s does not belong to this "
                                "vault",
                                v->key_path);
  }
  return ASVAULT_OK;
}

/* ---- sealing ------------------------------------------------------------ */

static core_err aad_str(core_buf *b, const char *s) {
  size_t n = strlen(s);
  uint8_t len[4];
  core_err ce;
  len[0] = (uint8_t)n;
  len[1] = (uint8_t)(n >> 8);
  len[2] = (uint8_t)(n >> 16);
  len[3] = (uint8_t)(n >> 24);
  ce = core_buf_append(b, len, sizeof len);
  return ce == CORE_OK ? core_buf_append(b, s, n) : ce;
}

static core_err aad_count(core_buf *b, size_t n) {
  uint8_t len[4];
  len[0] = (uint8_t)n;
  len[1] = (uint8_t)(n >> 8);
  len[2] = (uint8_t)(n >> 16);
  len[3] = (uint8_t)(n >> 24);
  return core_buf_append(b, len, sizeof len);
}

static asvault_err rec_aad(asvault *v, const asvault_rec *r, core_buf *b) {
  core_err ce;
  size_t i;
  core_buf_init(b);
  ce = core_buf_append(b, AAD_LABEL, sizeof AAD_LABEL); /* with the NUL */
  if (ce == CORE_OK) ce = aad_str(b, r->name);
  if (ce == CORE_OK) ce = aad_count(b, r->n_tools);
  for (i = 0; ce == CORE_OK && i < r->n_tools; i++) ce = aad_str(b, r->tools[i]);
  if (ce == CORE_OK) ce = aad_count(b, r->n_domains);
  for (i = 0; ce == CORE_OK && i < r->n_domains; i++)
    ce = aad_str(b, r->domains[i]);
  if (ce != CORE_OK) {
    core_buf_free(b);
    return asvault_seterr(v, ASVAULT_ERR_NOMEM, "out of memory");
  }
  return ASVAULT_OK;
}

asvault_err asvault_rec_seal(asvault *v, asvault_rec *r, const char *value,
                             size_t len) {
  core_buf aad;
  uint8_t *sealed;
  asvault_err e;

  if (!v->unlocked)
    return asvault_seterr(v, ASVAULT_ERR_LOCKED, "the vault is locked");
  e = rec_aad(v, r, &aad);
  if (e != ASVAULT_OK) return e;
  sealed = (uint8_t *)malloc(len + ASVAULT_TAG_SIZE);
  if (!sealed) {
    core_buf_free(&aad);
    return asvault_seterr(v, ASVAULT_ERR_NOMEM, "out of memory");
  }
  os_random_bytes(r->nonce, sizeof r->nonce);
  asvault_aead_seal(v->entry_key, r->nonce, (const uint8_t *)aad.data, aad.len,
                    (const uint8_t *)value, len, sealed);
  core_buf_free(&aad);
  free(r->sealed);
  r->sealed = sealed;
  r->sealed_len = len + ASVAULT_TAG_SIZE;
  return ASVAULT_OK;
}

asvault_err asvault_rec_open(asvault *v, const asvault_rec *r, char **out,
                             size_t *out_len) {
  core_buf aad;
  size_t len;
  char *pt;
  bool ok;
  asvault_err e;

  *out = NULL;
  if (out_len) *out_len = 0;
  if (!v->unlocked)
    return asvault_seterr(v, ASVAULT_ERR_LOCKED, "the vault is locked");
  if (!r->sealed || r->sealed_len <= ASVAULT_TAG_SIZE)
    return asvault_seterr(v, ASVAULT_ERR_CRYPTO,
                          "secret '%s' has no ciphertext", r->name);
  e = rec_aad(v, r, &aad);
  if (e != ASVAULT_OK) return e;
  len = r->sealed_len - ASVAULT_TAG_SIZE;
  pt = (char *)malloc(len + 1);
  if (!pt) {
    core_buf_free(&aad);
    return asvault_seterr(v, ASVAULT_ERR_NOMEM, "out of memory");
  }
  ok = asvault_aead_open(v->entry_key, r->nonce, (const uint8_t *)aad.data,
                         aad.len, r->sealed, r->sealed_len, (uint8_t *)pt);
  core_buf_free(&aad);
  if (!ok || memchr(pt, '\0', len) != NULL) {
    asvault_wipe(pt, len + 1);
    free(pt);
    return asvault_seterr(v, ASVAULT_ERR_CRYPTO,
                          "secret '%s' failed authentication: its name, "
                          "policy or ciphertext was altered",
                          r->name);
  }
  pt[len] = '\0';
  *out = pt;
  if (out_len) *out_len = len;
  return ASVAULT_OK;
}
