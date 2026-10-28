/* Credential record, locks and OAuth exchanges shared by the ChatGPT login
 * flow (chatgpt_login.c) and inference token access (chatgpt_auth.c).
 * Token values never reach logs or diagnostics. */
#ifndef ASMODEL_CHATGPT_AUTH_H
#define ASMODEL_CHATGPT_AUTH_H
#include "asmodel.h"
#include "core.h"
#include <stdio.h>

#define CG_LOGIN_HINT "run `asterism login chatgpt`"
#define CG_DYNAMIC_CLIENT "dynamic_agent_client"
#define CG_PLAN_SCOPE "chatgpt.tokens.use.direct"
#define CG_SCOPES "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct"
#define CG_TOKEN_PATH "/api/accounts/oauth/token"
#define CG_AUTHORIZE_PATH "/api/accounts/authorize"
#define CG_CALLBACK_PATH "/auth/callback"

typedef struct {
  char *issuer, *resource, *subject, *email, *plan, *client_id, *host_id;
  char *id_token, *access_token, *refresh_token, *token_type;
  char *scope; /* space-separated; stored as the "scopes" array */
  int64_t expires_at, earliest_refresh_at, saved_at;
} cg_record;

/* Parsed token-endpoint answer. */
typedef struct {
  char *access_token, *refresh_token, *id_token, *token_type, *scope;
  int64_t expires_in, earliest_refresh_at;
} cg_tokens;

/* Validated ID-token claims. */
typedef struct {
  char *subject, *email, *plan;
} cg_identity;

typedef struct { FILE *file; int held; } cg_lock;

void cg_record_free(cg_record *r);
void cg_tokens_free(cg_tokens *t);
void cg_identity_free(cg_identity *id);
/* NOT_FOUND when missing; INVALID when unreadable or corrupt. */
asmodel_err cg_record_read(const char *path, cg_record *r);
/* Atomic replace, owner-only (0600) on POSIX. */
asmodel_err cg_record_write(const char *path, const cg_record *r, char *error, size_t error_size);
void cg_record_clear_tokens(cg_record *r);
/* path + suffix, malloc'd. */
char *cg_path_with(const char *path, const char *suffix);
/* Process mutex plus the exclusive lock file <auth_file>.lock. */
asmodel_err cg_lock_acquire(const char *auth_file, int64_t timeout_ms, volatile int *cancel,
                            cg_lock *out, char *error, size_t error_size);
void cg_lock_release(cg_lock *l);
/* Reads (and with create, first creates) the stable ext_agent_host_id. */
asmodel_err cg_host_id(const char *host_file, int create, char **out, char *error, size_t error_size);
char *cg_random_token(size_t bytes);
char *cg_base64url(const uint8_t *data, size_t n);
int cg_has_scope(const char *scopes, const char *want);
core_err cg_form_add(core_buf *b, const char *key, const char *value);
/* POST form to {issuer}/api/accounts/oauth/token. On an OAuth error the
 * machine-readable code is copied to err_code; *terminal tells whether it
 * means the grant is unusable (invalid_grant, refresh_token_reused, ...). */
asmodel_err cg_token_request(const char *issuer, const char *form, int64_t timeout_ms,
                             volatile int *cancel, cg_tokens *out, char *err_code,
                             size_t err_code_size, int *terminal, char *error, size_t error_size);
/* iss, aud (client_id), exp and, when expected_nonce is set, nonce. The
 * signature is not verified: the token comes straight from the token endpoint
 * over TLS (OpenID Connect Core 1.0, section 3.1.3.7). */
asmodel_err cg_validate_id_token(const char *jwt, const char *issuer, const char *client_id,
                                 const char *expected_nonce, cg_identity *out,
                                 char *error, size_t error_size);
/* Fill a record from a successful token response. */
asmodel_err cg_record_apply(cg_record *r, cg_tokens *t, const cg_identity *id);
/* A string claim of an (unverified) JWT payload, malloc'd; NULL if absent. */
char *cg_jwt_claim(const char *jwt, const char *claim);
int cg_same_issuer(const char *a, const char *b);
#endif
