/*
 * api.c — the public API funnel: error codes and messages, open/close, and
 * the entry operations (set, delete, list, get).
 *
 * Every public call holds v->mu for its whole duration. Writers also hold
 * vault.lock (core's exclusive store lock) from load to atomic replace, so
 * concurrent handles and processes (the daemon, "asterism vault") never
 * lose each other's updates.
 */

#include "asvault_internal.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ---- errors ------------------------------------------------------------- */

static const char *const err_names[] = {
    "ASVAULT_OK",          "ASVAULT_ERR_INVALID",   "ASVAULT_ERR_NOMEM",
    "ASVAULT_ERR_IO",      "ASVAULT_ERR_PARSE",     "ASVAULT_ERR_NOT_FOUND",
    "ASVAULT_ERR_DENIED",  "ASVAULT_ERR_CRYPTO",    "ASVAULT_ERR_LOCKED",
};

const char *asvault_err_name(asvault_err e) {
  if ((int)e < 0 || (size_t)e >= sizeof err_names / sizeof err_names[0])
    return "ASVAULT_ERR_UNKNOWN";
  return err_names[e];
}

asvault_err asvault_from_core(core_err e) {
  switch (e) {
    case CORE_OK: return ASVAULT_OK;
    case CORE_ERR_NOT_FOUND: return ASVAULT_ERR_NOT_FOUND;
    case CORE_ERR_INVALID: return ASVAULT_ERR_INVALID;
    case CORE_ERR_NOMEM: return ASVAULT_ERR_NOMEM;
    case CORE_ERR_BUSY:
    case CORE_ERR_IO:
    default: return ASVAULT_ERR_IO;
  }
}

asvault_err asvault_seterr(asvault *v, asvault_err e, const char *fmt, ...) {
  va_list ap;
  if (!v || !fmt) return e;
  va_start(ap, fmt);
  vsnprintf(v->err, sizeof v->err, fmt, ap);
  va_end(ap);
  return e;
}

const char *asvault_last_error(const asvault *v) { return v ? v->err : ""; }

void asvault_free_secret(char *s) {
  if (!s) return;
  asvault_wipe(s, strlen(s));
  free(s);
}

/* ---- open / close ------------------------------------------------------- */

asvault_err asvault_open(const char *vault_dir, asvault **out) {
  asvault *v;
  if (!out) return ASVAULT_ERR_INVALID;
  *out = NULL;
  if (!vault_dir || !*vault_dir) return ASVAULT_ERR_INVALID;
  v = (asvault *)calloc(1, sizeof *v);
  if (!v) return ASVAULT_ERR_NOMEM;
  v->dir = core_strdup(vault_dir);
  v->file_path = os_path_join(vault_dir, "vault.xcdn");
  v->key_path = os_path_join(vault_dir, "vault.key");
  v->lock_path = os_path_join(vault_dir, "vault.lock");
  if (!v->dir || !v->file_path || !v->key_path || !v->lock_path) {
    free(v->dir);
    free(v->file_path);
    free(v->key_path);
    free(v->lock_path);
    free(v);
    return ASVAULT_ERR_NOMEM;
  }
  os_mutex_init(&v->mu);
  *out = v;
  return ASVAULT_OK;
}

void asvault_close(asvault *v) {
  if (!v) return;
  os_mutex_destroy(&v->mu);
  free(v->dir);
  free(v->file_path);
  free(v->key_path);
  free(v->lock_path);
  asvault_wipe(v, sizeof *v);
  free(v);
}

/* ---- writer lock -------------------------------------------------------- */

/* vault.lock, waiting up to five seconds for another writer. */
static asvault_err writer_lock(asvault *v, FILE **out) {
  int tries;
  for (tries = 0; tries < 50; tries++) {
    FILE *f = os_store_lock(v->lock_path);
    if (f) {
      *out = f;
      return ASVAULT_OK;
    }
    os_sleep_ms(100);
  }
  return asvault_seterr(v, ASVAULT_ERR_IO,
                        "cannot take the vault writer lock %s (another "
                        "writer holds it, or it is not a regular file)",
                        v->lock_path);
}

/* ---- set / delete ------------------------------------------------------- */

static bool name_ok(asvault *v, const char *name, asvault_err *e) {
  if (name && core_slug_valid(name)) return true;
  *e = asvault_seterr(v, ASVAULT_ERR_INVALID,
                      "secret names are slugs [a-z0-9][a-z0-9-]{0,63}");
  return false;
}

static asvault_err set_locked(asvault *v, const char *name, const char *value,
                              const asvault_policy *policy) {
  char **tools = NULL, **domains = NULL;
  size_t n_tools = 0, n_domains = 0, len;
  char now[CORE_RFC3339_SIZE];
  asvault_doc d;
  asvault_rec *r;
  FILE *lock = NULL;
  asvault_err e;

  memset(&d, 0, sizeof d);
  if (!name_ok(v, name, &e)) return e;
  if (!value)
    return asvault_seterr(v, ASVAULT_ERR_INVALID, "secret value missing");
  for (len = 0; len <= ASVAULT_VALUE_MAX && value[len] != '\0'; len++) {
  }
  if (len > ASVAULT_VALUE_MAX)
    return asvault_seterr(v, ASVAULT_ERR_INVALID,
                          "secret value longer than %d bytes",
                          ASVAULT_VALUE_MAX);
  if (len == 0)
    return asvault_seterr(v, ASVAULT_ERR_INVALID, "secret value is empty");
  if (!core_utf8_valid(value, len))
    return asvault_seterr(v, ASVAULT_ERR_INVALID,
                          "secret value is not valid UTF-8");
  e = asvault_policy_copy(v, policy, &tools, &n_tools, &domains, &n_domains);
  if (e != ASVAULT_OK) return e;

  e = os_private_dir(v->dir);
  if (e != ASVAULT_OK) {
    e = asvault_seterr(v, e, "cannot create the vault directory %s", v->dir);
    goto done;
  }
  e = writer_lock(v, &lock);
  if (e != ASVAULT_OK) goto done;
  e = asvault_doc_load(v, &d);
  if (e != ASVAULT_OK) goto done;
  e = d.exists ? asvault_key_unlock(v, &d) : asvault_key_create(v, &d);
  if (e != ASVAULT_OK) goto done;

  core_time_format_rfc3339((core_time)os_now_unix(), now);
  r = asvault_doc_find(&d, name);
  if (!r) {
    e = asvault_doc_append(v, &d, name, &r);
    if (e != ASVAULT_OK) goto done;
    memcpy(r->created_at, now, sizeof now);
  }
  memcpy(r->updated_at, now, sizeof now);
  asvault_strv_free(r->tools, r->n_tools);
  asvault_strv_free(r->domains, r->n_domains);
  r->tools = tools;
  r->n_tools = n_tools;
  r->domains = domains;
  r->n_domains = n_domains;
  tools = domains = NULL; /* owned by the record */
  e = asvault_rec_seal(v, r, value, len);
  if (e == ASVAULT_OK) e = asvault_doc_save(v, &d);

done:
  if (lock) fclose(lock);
  asvault_strv_free(tools, n_tools);
  asvault_strv_free(domains, n_domains);
  asvault_doc_free(&d);
  return e;
}

asvault_err asvault_set(asvault *v, const char *name, const char *value,
                        const asvault_policy *policy) {
  asvault_err e;
  if (!v) return ASVAULT_ERR_INVALID;
  os_mutex_lock(&v->mu);
  e = set_locked(v, name, value, policy);
  os_mutex_unlock(&v->mu);
  return e;
}

static asvault_err delete_locked(asvault *v, const char *name) {
  asvault_doc d;
  asvault_rec *r;
  FILE *lock = NULL;
  asvault_err e;

  memset(&d, 0, sizeof d);
  if (!name_ok(v, name, &e)) return e;
  if (!os_file_exists(v->file_path))
    return asvault_seterr(v, ASVAULT_ERR_NOT_FOUND, "unknown secret '%s'",
                          name);
  e = writer_lock(v, &lock);
  if (e != ASVAULT_OK) return e;
  e = asvault_doc_load(v, &d);
  if (e == ASVAULT_OK) {
    r = d.exists ? asvault_doc_find(&d, name) : NULL;
    if (!r) {
      e = asvault_seterr(v, ASVAULT_ERR_NOT_FOUND, "unknown secret '%s'",
                         name);
    } else {
      asvault_doc_remove(&d, r);
      e = asvault_doc_save(v, &d);
    }
  }
  fclose(lock);
  asvault_doc_free(&d);
  return e;
}

asvault_err asvault_delete(asvault *v, const char *name) {
  asvault_err e;
  if (!v) return ASVAULT_ERR_INVALID;
  os_mutex_lock(&v->mu);
  e = delete_locked(v, name);
  os_mutex_unlock(&v->mu);
  return e;
}

/* ---- list --------------------------------------------------------------- */

void asvault_list_free(asvault_entry_info *list, size_t n) {
  size_t i;
  if (!list) return;
  for (i = 0; i < n; i++) {
    asvault_strv_free(list[i].tools, list[i].n_tools);
    asvault_strv_free(list[i].domains, list[i].n_domains);
  }
  free(list);
}

static int info_cmp(const void *a, const void *b) {
  return strcmp(((const asvault_entry_info *)a)->name,
                ((const asvault_entry_info *)b)->name);
}

static asvault_err list_locked(asvault *v, asvault_entry_info **out,
                               size_t *out_n) {
  asvault_entry_info *list;
  asvault_doc d;
  size_t i;
  asvault_err e;

  e = asvault_doc_load(v, &d);
  if (e != ASVAULT_OK) return e;
  list = (asvault_entry_info *)calloc(d.n ? d.n : 1, sizeof *list);
  if (!list) {
    asvault_doc_free(&d);
    return asvault_seterr(v, ASVAULT_ERR_NOMEM, "out of memory");
  }
  for (i = 0; i < d.n; i++) {
    const asvault_rec *r = &d.recs[i];
    asvault_entry_info *x = &list[i];
    memcpy(x->name, r->name, sizeof x->name);
    snprintf(x->updated_at, sizeof x->updated_at, "%s", r->updated_at);
    x->tools = asvault_strv_dup(r->tools, r->n_tools);
    x->domains = asvault_strv_dup(r->domains, r->n_domains);
    if (!x->tools || !x->domains) {
      asvault_list_free(list, i + 1);
      asvault_doc_free(&d);
      return asvault_seterr(v, ASVAULT_ERR_NOMEM, "out of memory");
    }
    x->n_tools = r->n_tools;
    x->n_domains = r->n_domains;
  }
  if (d.n > 1) qsort(list, d.n, sizeof *list, info_cmp);
  *out = list;
  *out_n = d.n;
  asvault_doc_free(&d);
  return ASVAULT_OK;
}

asvault_err asvault_list(asvault *v, asvault_entry_info **out, size_t *out_n) {
  asvault_err e;
  if (out) *out = NULL;
  if (out_n) *out_n = 0;
  if (!v || !out || !out_n) return ASVAULT_ERR_INVALID;
  os_mutex_lock(&v->mu);
  e = list_locked(v, out, out_n);
  os_mutex_unlock(&v->mu);
  return e;
}

/* ---- get ---------------------------------------------------------------- */

static asvault_err get_locked(asvault *v, const char *name,
                              const char *consumer, char **out_value) {
  asvault_doc d;
  const asvault_rec *r;
  asvault_err e;

  if (!name_ok(v, name, &e)) return e;
  if (!consumer || !core_slug_valid(consumer))
    return asvault_seterr(v, ASVAULT_ERR_INVALID,
                          "consumer tool id must be a slug");
  e = asvault_doc_load(v, &d);
  if (e != ASVAULT_OK) return e;
  r = asvault_doc_find(&d, name);
  if (!r) {
    e = asvault_seterr(v, ASVAULT_ERR_NOT_FOUND, "unknown secret '%s'", name);
  } else if (!asvault_rec_allows_tool(r, consumer)) {
    e = asvault_seterr(v, ASVAULT_ERR_DENIED,
                       "secret '%s' is not allowed for tool '%s'", name,
                       consumer);
  } else {
    e = asvault_key_unlock(v, &d);
    if (e == ASVAULT_OK) e = asvault_rec_open(v, r, out_value, NULL);
  }
  asvault_doc_free(&d);
  return e;
}

asvault_err asvault_get(asvault *v, const char *name,
                        const char *consumer_tool, char **out_value) {
  asvault_err e;
  if (out_value) *out_value = NULL;
  if (!v || !out_value) return ASVAULT_ERR_INVALID;
  os_mutex_lock(&v->mu);
  e = get_locked(v, name, consumer_tool, out_value);
  os_mutex_unlock(&v->mu);
  return e;
}
