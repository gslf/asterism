/*
 * asvault_internal.h — internal contracts of libasvault.
 *
 * Module map (one .c per row; a module implements exactly the functions
 * declared in its section plus static helpers):
 *
 *   crypto.c       ChaCha20, Poly1305, AEAD (RFC 8439), PBKDF2 (RFC 8018)
 *   policy.c       tool and domain validation, normalization, matching
 *   store.c        vault.xcdn parse/serialize, master key, entry sealing
 *   placeholder.c  {{secret:NAME}} scanning and resolution
 *   redact.c       stored values (and encodings) back to placeholders
 *   api.c          public API funnel: errors, open/close, set/get/list
 *   cli.c          "asterism vault ..."
 *   os_private.c   owner-only files and no-echo terminal input (os.h)
 *
 * Locking protocol: v->mu guards the whole handle (cached key, last error)
 * for the duration of every public call. Writers additionally hold the
 * vault.lock file lock across processes from load to atomic replace;
 * readers need no file lock because vault.xcdn is only ever replaced
 * atomically.
 *
 * Secret hygiene: plaintext values and key material live in buffers this
 * module owns and are zeroized with asvault_wipe before release. Buffers
 * holding plaintext are sized once (never grown by realloc, which would
 * leave copies behind).
 */

#ifndef ASVAULT_INTERNAL_H
#define ASVAULT_INTERNAL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "asvault.h"
#include "os.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ASVAULT_KEY_SIZE   32
#define ASVAULT_NONCE_SIZE 12
#define ASVAULT_TAG_SIZE   16
#define ASVAULT_SALT_SIZE  16
#define ASVAULT_KDF_MAX_ITERATIONS 100000000u
#define ASVAULT_DOMAIN_MAX 253

/* ═══════════════════════════ handle (api.c) ═══════════════════════════ */

struct asvault {
  os_mutex mu;
  char    *dir;
  char    *file_path; /* vault.xcdn */
  char    *key_path;  /* vault.key  */
  char    *lock_path; /* vault.lock */
  char     err[512];
  int      unlocked;                    /* entry_key is valid for check */
  uint8_t  check[32];                   /* key-check value of the vault  */
  uint8_t  entry_key[ASVAULT_KEY_SIZE]; /* AEAD key for entries          */
};

/* Record a message on the handle (never a value) and return e. */
asvault_err asvault_seterr(asvault *v, asvault_err e, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

/* ═══════════════════════════ crypto.c ═══════════════════════════ */

/* Zeroize n bytes; never elided by the optimizer. */
void asvault_wipe(void *p, size_t n);

void asvault_chacha20_quarter_round(uint32_t x[16], int a, int b, int c,
                                    int d);
void asvault_chacha20_block(const uint8_t key[32], uint32_t counter,
                            const uint8_t nonce[12], uint8_t out[64]);
/* out = in XOR keystream starting at block `counter`; in may equal out. */
void asvault_chacha20_xor(const uint8_t key[32], uint32_t counter,
                          const uint8_t nonce[12], const uint8_t *in,
                          uint8_t *out, size_t len);

typedef struct {
  uint32_t r[5];
  uint32_t h[5];
  uint32_t pad[4];
  size_t   leftover;
  uint8_t  buffer[16];
  int      final;
} asvault_poly1305;

void asvault_poly1305_init(asvault_poly1305 *st, const uint8_t key[32]);
void asvault_poly1305_update(asvault_poly1305 *st, const uint8_t *m, size_t n);
/* Writes the tag and zeroizes the state. */
void asvault_poly1305_final(asvault_poly1305 *st, uint8_t tag[16]);
void asvault_poly1305_mac(const uint8_t key[32], const uint8_t *m, size_t n,
                          uint8_t tag[16]);
/* One-time Poly1305 key: first 32 bytes of ChaCha20 block 0 (RFC 8439 2.6). */
void asvault_poly1305_keygen(const uint8_t key[32], const uint8_t nonce[12],
                             uint8_t out[32]);

/* AEAD_CHACHA20_POLY1305: out receives len ciphertext bytes then the tag. */
void asvault_aead_seal(const uint8_t key[32], const uint8_t nonce[12],
                       const uint8_t *aad, size_t aad_len, const uint8_t *pt,
                       size_t len, uint8_t *out);
/* in is ciphertext || tag (in_len >= 16); out receives in_len - 16 bytes,
 * written only when the tag verifies (constant-time compare). */
bool asvault_aead_open(const uint8_t key[32], const uint8_t nonce[12],
                       const uint8_t *aad, size_t aad_len, const uint8_t *in,
                       size_t in_len, uint8_t *out);

/* PBKDF2-HMAC-SHA256; false when iterations is 0 or salt exceeds 128 bytes. */
bool asvault_pbkdf2_sha256(const void *pw, size_t pw_len, const uint8_t *salt,
                           size_t salt_len, uint32_t iterations, uint8_t *out,
                           size_t out_len);

/* ═══════════════════════════ policy.c ═══════════════════════════ */

/* Normalize a domain (ASCII lowercase, one trailing dot dropped) into out;
 * false when it is not a host name ("*." prefix allowed with wildcard). */
bool asvault_domain_normalize(const char *in, bool wildcard,
                              char out[ASVAULT_DOMAIN_MAX + 1]);
/* pattern is normalized ("host" or "*.suffix"); host is normalized. */
bool asvault_domain_match(const char *pattern, const char *host);
/* Sorted, deduplicated, validated copies of a policy's lists. */
asvault_err asvault_policy_copy(asvault *v, const asvault_policy *p,
                                char ***tools, size_t *n_tools,
                                char ***domains, size_t *n_domains);
char **asvault_strv_dup(char *const *src, size_t n); /* NULL on failure */
void   asvault_strv_free(char **v, size_t n);

/* ═══════════════════════════ store.c ═══════════════════════════ */

typedef struct {
  char     name[ASVAULT_NAME_MAX + 1];
  char     created_at[CORE_RFC3339_SIZE];
  char     updated_at[CORE_RFC3339_SIZE];
  char   **tools;
  size_t   n_tools;
  char   **domains;
  size_t   n_domains;
  uint8_t  nonce[ASVAULT_NONCE_SIZE];
  uint8_t *sealed; /* ciphertext || tag */
  size_t   sealed_len;
} asvault_rec;

typedef enum { ASVAULT_MODE_KEYFILE = 0, ASVAULT_MODE_PASSPHRASE } asvault_mode;

typedef struct {
  int          exists; /* vault.xcdn was present */
  char         created_at[CORE_RFC3339_SIZE];
  asvault_mode mode;
  uint8_t      salt[ASVAULT_SALT_SIZE];
  uint32_t     iterations;
  uint8_t      check[32];
  asvault_rec *recs;
  size_t       n;
} asvault_doc;

/* Load vault.xcdn; a missing file is an empty doc with exists = 0.
 * ASVAULT_ERR_PARSE for anything malformed. */
asvault_err  asvault_doc_load(asvault *v, asvault_doc *d);
/* Serialize (entries sorted by name) and replace vault.xcdn atomically. */
asvault_err  asvault_doc_save(asvault *v, asvault_doc *d);
void         asvault_doc_free(asvault_doc *d);
asvault_rec *asvault_doc_find(const asvault_doc *d, const char *name);
/* Append an empty record (policy and sealed value still to fill). */
asvault_err  asvault_doc_append(asvault *v, asvault_doc *d, const char *name,
                                asvault_rec **out);
void         asvault_doc_remove(asvault_doc *d, asvault_rec *r);
void         asvault_rec_free(asvault_rec *r);

/* New vault: choose the key mode (passphrase when ASTERISM_VAULT_PASSPHRASE
 * is set, else the key file, created if missing), fill the header and cache
 * the entry key. */
asvault_err asvault_key_create(asvault *v, asvault_doc *d);
/* Existing vault: make v->entry_key valid for d (cached when d->check is
 * unchanged); ASVAULT_ERR_LOCKED when the key cannot be obtained. */
asvault_err asvault_key_unlock(asvault *v, const asvault_doc *d);

bool asvault_rec_allows_tool(const asvault_rec *r, const char *tool);
/* host is normalized or NULL; a record without domains allows any. */
bool asvault_rec_allows_domain(const asvault_rec *r, const char *host);
/* Encrypt value under a fresh nonce, bound to r's name and policy. */
asvault_err asvault_rec_seal(asvault *v, asvault_rec *r, const char *value,
                             size_t len);
/* Decrypt into a malloc'd NUL-terminated buffer; release with
 * asvault_free_secret. ASVAULT_ERR_CRYPTO when authentication fails. */
asvault_err asvault_rec_open(asvault *v, const asvault_rec *r, char **out,
                             size_t *out_len);

/* ═══════════════════════════ placeholder.c ═══════════════════════════ */

typedef struct {
  size_t start, end; /* byte span of "{{secret:NAME}}" */
  char   name[ASVAULT_NAME_MAX + 1];
} asvault_ph;

/* Every placeholder in text order; ASVAULT_ERR_INVALID with *bad_at for a
 * malformed one. *out is NULL when there are none. */
asvault_err asvault_ph_scan(const char *text, asvault_ph **out, size_t *out_n,
                            size_t *bad_at);

#ifdef __cplusplus
}
#endif

#endif /* ASVAULT_INTERNAL_H */
