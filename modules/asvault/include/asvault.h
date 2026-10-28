/*
 * asvault.h — public C API of libasvault, the local secret vault.
 *
 * Secrets (bot tokens, IMAP passwords, API keys) are stored encrypted under
 * <engine-root>/vault/ and never enter a model prompt. The model and the
 * transcripts only ever see the surrogate placeholder {{secret:NAME}}: the
 * host resolves it into the real value at the tool boundary, for a tool
 * (and optionally a domain) the secret's policy allows, and redacts real
 * values out of tool output before the model sees it.
 *
 * Layout of the vault directory (0700 on POSIX):
 *   vault.xcdn  header + entries, rewritten by atomic replace (0600)
 *   vault.key   32 random bytes, the master key in key-file mode (0600)
 *   vault.lock  single-writer lock across processes
 * Each entry is ChaCha20-Poly1305 (RFC 8439) under a key derived from the
 * master key, with a fresh random nonce per write; the associated data
 * binds the entry name and its policy, so moving a ciphertext to another
 * name or editing the policy makes the entry fail authentication. When
 * ASTERISM_VAULT_PASSPHRASE is set while the vault is created, the master
 * key is PBKDF2-HMAC-SHA256 of the passphrase instead (salt and iterations
 * in the header) and no key file is written.
 *
 * Contract notes:
 *  - All strings are UTF-8 and NUL-terminated. Secret names and tool ids
 *    are slugs [a-z0-9][a-z0-9-]{0,63}; domains are host names, optionally
 *    "*.example.com" for any subdomain, compared case-insensitively.
 *  - A handle is thread-safe. Several handles and processes may share one
 *    directory: every call reads the current files and writers are
 *    serialized by vault.lock.
 *  - Strings that carry or derive from secret material (asvault_get,
 *    asvault_resolve, asvault_redact) are freed with asvault_free_secret,
 *    which zeroizes them. Nothing here logs or prints a value.
 *  - Errors set a message readable with asvault_last_error; it names the
 *    offending secret, tool or domain but never a value.
 *
 * MIT License — see LICENSE.
 */

#ifndef ASVAULT_H
#define ASVAULT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASVAULT_NAME_MAX       64      /* bytes in a secret name           */
#define ASVAULT_VALUE_MAX      65536   /* bytes in a secret value          */
#define ASVAULT_REDACT_MIN     6       /* shorter values are not redacted  */
#define ASVAULT_KDF_ITERATIONS 600000  /* PBKDF2 minimum and default       */
#define ASVAULT_PASSPHRASE_ENV "ASTERISM_VAULT_PASSPHRASE"

typedef struct asvault asvault;

typedef enum {
  ASVAULT_OK = 0,
  ASVAULT_ERR_INVALID,   /* bad argument, name, policy or placeholder     */
  ASVAULT_ERR_NOMEM,
  ASVAULT_ERR_IO,        /* file system failure, or the writer lock busy  */
  ASVAULT_ERR_PARSE,     /* vault.xcdn is corrupt: never overwritten      */
  ASVAULT_ERR_NOT_FOUND, /* unknown secret                                 */
  ASVAULT_ERR_DENIED,    /* the policy does not allow this tool or domain */
  ASVAULT_ERR_CRYPTO,    /* an entry failed authentication (tampered)     */
  ASVAULT_ERR_LOCKED     /* cannot unlock: passphrase or key file missing,
                            wrong, or not this vault's                    */
} asvault_err;

/* Stable identifier of an error code ("ASVAULT_ERR_DENIED", ...). */
const char *asvault_err_name(asvault_err e);

/* Opens a handle on vault_dir. Nothing is read or created here: the files
 * appear on the first asvault_set. */
asvault_err asvault_open(const char *vault_dir, asvault **out);
void        asvault_close(asvault *v); /* zeroizes the cached key */

/* Message of the last failing call on this handle ("" when none); valid
 * until the next call on the handle. */
const char *asvault_last_error(const asvault *v);

/* Who may receive a secret: tool package ids (at least one) and, when
 * n_domains > 0, the only domains an invocation may target with it. */
typedef struct {
  const char *const *tools;
  size_t             n_tools;
  const char *const *domains;
  size_t             n_domains;
} asvault_policy;

/* Stores or replaces a secret. The value is non-empty UTF-8 of at most
 * ASVAULT_VALUE_MAX bytes; the policy is stored sorted and deduplicated. */
asvault_err asvault_set(asvault *v, const char *name, const char *value,
                        const asvault_policy *policy);
asvault_err asvault_delete(asvault *v, const char *name);

/* Metadata of one entry; never contains the value. */
typedef struct {
  char    name[ASVAULT_NAME_MAX + 1];
  char    updated_at[32]; /* RFC 3339, UTC */
  char  **tools;
  size_t  n_tools;
  char  **domains;
  size_t  n_domains;
} asvault_entry_info;

/* Every entry, sorted by name; needs no key. */
asvault_err asvault_list(asvault *v, asvault_entry_info **out, size_t *out_n);
void        asvault_list_free(asvault_entry_info *list, size_t n);

/* Host-only: the real value for a named consumer (e.g. the Telegram channel
 * inside the daemon). ASVAULT_ERR_DENIED unless consumer_tool is in the
 * secret's policy.tools. Free with asvault_free_secret. */
asvault_err asvault_get(asvault *v, const char *name,
                        const char *consumer_tool, char **out_value);
/* Zeroizes and frees a string returned by this API; NULL is fine. */
void        asvault_free_secret(char *s);

/* Replaces every {{secret:NAME}} in text for an invocation of tool tool_id,
 * optionally targeting domain (NULL = none). Every placeholder is checked
 * before anything is substituted: ASVAULT_ERR_NOT_FOUND for an unknown
 * name, ASVAULT_ERR_DENIED when the tool, or the domain of a
 * domain-restricted secret (a NULL domain included), is not allowed,
 * ASVAULT_ERR_INVALID for a malformed placeholder ("{{secret:" not followed
 * by a slug and "}}"); last_error names the offender. Free the output with
 * asvault_free_secret. */
asvault_err asvault_resolve(asvault *v, const char *text, const char *tool_id,
                            const char *domain, char **out_text);

/* Replaces every occurrence of any stored value in text with
 * {{secret:NAME}}: the value as-is, base64 (standard and URL-safe, padded or
 * not, also inside a longer base64 run such as HTTP Basic credentials),
 * URL-encoded (either hex case, '+' for space) and JSON-escaped. Overlapping
 * matches are merged, so no byte of a matched value survives. Values shorter
 * than ASVAULT_REDACT_MIN bytes are not redacted ("vault check" reports
 * them). Needs the key: ASVAULT_ERR_LOCKED means nothing was redacted.
 * Used on tool output and outbound messages; free with asvault_free_secret. */
asvault_err asvault_redact(asvault *v, const char *text, char **out_text);

/* Distinct placeholder names referenced in text, in order of first
 * appearance (for approval previews); ASVAULT_ERR_INVALID for a malformed
 * placeholder. Free with asvault_free_names. */
asvault_err asvault_placeholders(const char *text, char ***out_names,
                                 size_t *out_n);
void        asvault_free_names(char **names, size_t n);

/* "asterism vault ...": argv[0] is the mode name, argv[1] the command.
 *   set <name> --tool <id> [--tool <id>]... [--domain <d>]...
 *       value from stdin (a TTY prompt does not echo it)
 *   list | rm <name> | check
 * Never prints a value. Returns 0 on success, 1 on failure (check: any
 * error found), 2 on usage errors. */
int asvault_cli_main(int argc, char **argv, const char *vault_dir);

#ifdef __cplusplus
}
#endif

#endif /* ASVAULT_H */
