/* Secrets at the tool boundary. The model, the approval record, the journal
 * and memory only ever hold {{secret:NAME}} placeholders; the real value is
 * substituted into the arguments of one invocation, for a tool (and domain)
 * the secret's vault policy names, and redacted out of what comes back. */
#include "asngn_internal.h"
#include "asvault.h"
#include <stdlib.h>
#include <string.h>

static char *vault_dir(asngn_ctx *c) {
  return c->root ? os_path_join(c->root, "vault") : NULL;
}

/* Host of the first http(s) URL in the arguments, for domain policies. */
static void url_host(const char *args, char host[256]) {
  const char *p = strstr(args, "://");
  size_t n = 0;
  host[0] = 0;
  if (!p) return;
  p += 3;
  while (p[n] && p[n] != '/' && p[n] != ':' && p[n] != '"' && p[n] != '?' &&
         p[n] != '#' && p[n] != ' ' && n < 255)
    n++;
  memcpy(host, p, n);
  host[n] = 0;
}

asngn_err asngn_secrets_resolve(asngn_ctx *c, const char *tool_ref, const char *args,
                                char **out, char *why, size_t why_size) {
  char **names = NULL, tool[64], host[256];
  size_t n = 0;
  asvault *v = NULL;
  asvault_err ve;
  *out = NULL;
  why[0] = 0;
  if (!args || !strstr(args, "{{secret:")) return ASNGN_OK;
  ve = asvault_placeholders(args, &names, &n);
  asvault_free_names(names, n);
  if (ve != ASVAULT_OK) {
    snprintf(why, why_size, "malformed secret placeholder: use {{secret:NAME}}");
    return ASNGN_ERR_DENIED;
  }
  snprintf(tool, sizeof tool, "%s", tool_ref);
  char *at = strchr(tool, '@');
  if (at) *at = 0;
  url_host(args, host);
  char *dir = vault_dir(c);
  ve = dir ? asvault_open(dir, &v) : ASVAULT_ERR_NOMEM;
  free(dir);
  if (ve == ASVAULT_OK) ve = asvault_resolve(v, args, tool, host[0] ? host : NULL, out);
  if (ve != ASVAULT_OK)
    snprintf(why, why_size, "secret not released to %s: %s", tool,
             v ? asvault_last_error(v) : asvault_err_name(ve));
  asvault_close(v);
  return ve == ASVAULT_OK ? ASNGN_OK : ASNGN_ERR_DENIED;
}

void asngn_secrets_free(char *s) { asvault_free_secret(s); }

/* Fails closed: when a vault exists but redaction fails, the text is
 * replaced by a notice rather than risk returning a secret. */
void asngn_secrets_redact(asngn_ctx *c, char **text) {
  char *dir, *file, *clean = NULL;
  asvault *v = NULL;
  if (!text || !*text) return;
  dir = vault_dir(c);
  file = dir ? os_path_join(dir, "vault.xcdn") : NULL;
  bool present = file && os_file_exists(file);
  free(file);
  if (!present) {
    free(dir);
    return;
  }
  asvault_err ve = asvault_open(dir, &v);
  free(dir);
  if (ve == ASVAULT_OK) ve = asvault_redact(v, *text, &clean);
  asvault_close(v);
  char *next = ve == ASVAULT_OK ? core_strdup(clean)
                                : core_strdup("[output withheld: secret redaction failed]");
  asvault_free_secret(clean);
  if (!next) return;
  /* astools results are plain malloc allocations. */
  memset(*text, 0, strlen(*text));
  free(*text);
  *text = next;
}
