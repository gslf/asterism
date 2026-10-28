/*
 * cli.c — "asterism vault": set, list, rm and check.
 *
 * set reads the value from standard input: one line without echo on a
 * terminal, else the whole input with one trailing newline dropped (so a
 * PEM key can be piped in). No command prints a value; check decrypts
 * every entry only to verify it and to measure its length.
 *
 * Exit codes: 0 success; 1 failure (check: any error found); 2 usage.
 */

#include "asvault_internal.h"

#include <stdlib.h>
#include <string.h>

static void usage(FILE *to) {
  fputs("usage: asterism vault set <name> --tool <id> [--tool <id>]...\n"
        "                          [--domain <host>]...\n"
        "         (the value is read from standard input, never echoed)\n"
        "       asterism vault list\n"
        "       asterism vault rm <name>\n"
        "       asterism vault check\n"
        "Tools and models see a secret only as {{secret:<name>}}.\n",
        to);
}

static int fail(asvault *v, asvault_err e) {
  const char *msg = v ? asvault_last_error(v) : NULL;
  fprintf(stderr, "error: %s\n", msg && *msg ? msg : asvault_err_name(e));
  return 1;
}

static void print_list(char *const *items, size_t n, const char *none) {
  size_t i;
  if (n == 0) fputs(none, stdout);
  for (i = 0; i < n; i++) printf("%s%s", i ? "," : "", items[i]);
}

/* ---- set ---------------------------------------------------------------- */

static int cmd_set(int argc, char **argv, const char *dir) {
  const size_t cap = ASVAULT_VALUE_MAX + 3; /* value, "\r\n", NUL */
  const char **tools, **domains;
  const char *name = NULL;
  asvault_policy policy;
  size_t nt = 0, nd = 0, len = 0, i;
  asvault *v = NULL;
  char *buf, prompt[128];
  int rc = 1;
  asvault_err e;

  tools = (const char **)calloc((size_t)argc + 1, sizeof *tools);
  domains = (const char **)calloc((size_t)argc + 1, sizeof *domains);
  if (!tools || !domains) {
    free(tools);
    free(domains);
    fputs("error: out of memory\n", stderr);
    return 1;
  }
  for (i = 0; i < (size_t)argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--tool") == 0 || strcmp(a, "--domain") == 0) {
      if (i + 1 >= (size_t)argc) goto bad_usage;
      if (a[2] == 't')
        tools[nt++] = argv[++i];
      else
        domains[nd++] = argv[++i];
    } else if (core_str_has_prefix(a, "--tool=")) {
      tools[nt++] = a + 7;
    } else if (core_str_has_prefix(a, "--domain=")) {
      domains[nd++] = a + 9;
    } else if (a[0] == '-' || name) {
      goto bad_usage;
    } else {
      name = a;
    }
  }
  if (!name) goto bad_usage;
  if (nt == 0) {
    fputs("error: set needs at least one --tool <id> allowed to receive "
          "the secret\n",
          stderr);
    goto bad_usage;
  }

  /* refuse a bad name, tool or domain before anyone types the value */
  if (!core_slug_valid(name)) {
    fputs("error: secret names are slugs [a-z0-9][a-z0-9-]{0,63}\n", stderr);
    free(tools);
    free(domains);
    return 1;
  }
  for (i = 0; i < nt + nd; i++) {
    char norm[ASVAULT_DOMAIN_MAX + 1];
    if (i < nt ? core_slug_valid(tools[i])
               : asvault_domain_normalize(domains[i - nt], true, norm))
      continue;
    if (i < nt)
      fprintf(stderr, "error: tool id \"%.64s\" is not a slug\n", tools[i]);
    else
      fprintf(stderr, "error: \"%.253s\" is not a domain name\n",
              domains[i - nt]);
    free(tools);
    free(domains);
    return 1;
  }
  e = asvault_open(dir, &v);
  if (e != ASVAULT_OK) {
    free(tools);
    free(domains);
    return fail(NULL, e);
  }
  buf = (char *)malloc(cap);
  if (!buf) {
    fputs("error: out of memory\n", stderr);
    goto out;
  }
  if (os_stdin_is_tty()) {
    snprintf(prompt, sizeof prompt, "Value for %s (input hidden): ", name);
    e = os_read_stdin(buf, cap, &len, prompt);
  } else {
    e = os_read_stdin(buf, cap, &len, NULL);
  }
  if (len > 0 && buf[len - 1] == '\n') len--;
  if (len > 0 && buf[len - 1] == '\r') len--;
  buf[len] = '\0';
  if (e == ASVAULT_ERR_INVALID || len > ASVAULT_VALUE_MAX) {
    fprintf(stderr, "error: the value is longer than %d bytes\n",
            ASVAULT_VALUE_MAX);
  } else if (e != ASVAULT_OK) {
    fputs("error: cannot read the value from standard input\n", stderr);
  } else if (strlen(buf) != len) {
    fputs("error: the value contains a NUL byte\n", stderr);
  } else if (len == 0) {
    fputs("error: empty value (pipe it in or type it at the prompt)\n",
          stderr);
  } else {
    policy.tools = tools;
    policy.n_tools = nt;
    policy.domains = domains;
    policy.n_domains = nd;
    e = asvault_set(v, name, buf, &policy);
    if (e != ASVAULT_OK) {
      fail(v, e);
    } else {
      printf("stored secret '%s' (tools: ", name);
      for (i = 0; i < nt; i++) printf("%s%s", i ? "," : "", tools[i]);
      fputs("; domains: ", stdout);
      if (nd == 0) fputs("any", stdout);
      for (i = 0; i < nd; i++) printf("%s%s", i ? "," : "", domains[i]);
      printf("); reference it as {{secret:%s}}\n", name);
      if (len < ASVAULT_REDACT_MIN)
        fprintf(stderr,
                "warning: values shorter than %d bytes are not redacted "
                "from tool output\n",
                ASVAULT_REDACT_MIN);
      rc = 0;
    }
  }
  asvault_wipe(buf, cap);
  free(buf);
out:
  asvault_close(v);
  free(tools);
  free(domains);
  return rc;

bad_usage:
  free(tools);
  free(domains);
  usage(stderr);
  return 2;
}

/* ---- list / rm ---------------------------------------------------------- */

static int cmd_list(const char *dir) {
  asvault_entry_info *list = NULL;
  size_t n = 0, i;
  asvault *v = NULL;
  asvault_err e = asvault_open(dir, &v);
  if (e != ASVAULT_OK) return fail(NULL, e);
  e = asvault_list(v, &list, &n);
  if (e != ASVAULT_OK) {
    int rc = fail(v, e);
    asvault_close(v);
    return rc;
  }
  if (n == 0) puts("no secrets");
  for (i = 0; i < n; i++) {
    printf("%-24s %s  tools: ", list[i].name, list[i].updated_at);
    print_list(list[i].tools, list[i].n_tools, "none");
    fputs("  domains: ", stdout);
    print_list(list[i].domains, list[i].n_domains, "any");
    fputc('\n', stdout);
  }
  asvault_list_free(list, n);
  asvault_close(v);
  return 0;
}

static int cmd_rm(const char *dir, const char *name) {
  asvault *v = NULL;
  asvault_err e = asvault_open(dir, &v);
  int rc = 0;
  if (e != ASVAULT_OK) return fail(NULL, e);
  e = asvault_delete(v, name);
  if (e != ASVAULT_OK)
    rc = fail(v, e);
  else
    printf("removed secret '%s'\n", name);
  asvault_close(v);
  return rc;
}

/* ---- check -------------------------------------------------------------- */

typedef struct {
  int errors, warnings;
} ck_tally;

/* Group/other bits on a vault file are an error; on the directory a
 * warning (the files inside stay owner-only). */
static void ck_mode(ck_tally *t, const char *path, unsigned want, int is_err) {
  unsigned mode = 0;
  if (!os_private_mode(path, &mode) || (mode & 077u) == 0) return;
  printf("%s %s is mode %04o, expected %04o\n", is_err ? "error  " : "warning",
         path, mode & 07777u, want);
  if (is_err)
    t->errors++;
  else
    t->warnings++;
}

static int cmd_check(const char *dir) {
  ck_tally t = {0, 0};
  asvault_doc d;
  asvault *v = NULL;
  size_t i;
  asvault_err e = asvault_open(dir, &v);

  if (e != ASVAULT_OK) return fail(NULL, e);
  os_mutex_lock(&v->mu);
  e = asvault_doc_load(v, &d);
  if (e != ASVAULT_OK) {
    os_mutex_unlock(&v->mu);
    printf("error   %s\n", asvault_last_error(v));
    asvault_close(v);
    return 1;
  }
  if (!d.exists) {
    printf("vault: empty (%s does not exist yet)\n", v->file_path);
    goto out;
  }
  ck_mode(&t, v->dir, 0700, 0);
  ck_mode(&t, v->file_path, 0600, 1);
  if (d.mode == ASVAULT_MODE_KEYFILE) ck_mode(&t, v->key_path, 0600, 1);
  if (d.mode == ASVAULT_MODE_PASSPHRASE)
    printf("vault: passphrase key (pbkdf2-hmac-sha256, %lu iterations)\n",
           (unsigned long)d.iterations);
  else
    printf("vault: key file %s\n", v->key_path);

  e = asvault_key_unlock(v, &d);
  if (e != ASVAULT_OK) {
    printf("error   %s\n", asvault_last_error(v));
    t.errors++;
    goto out;
  }
  for (i = 0; i < d.n; i++) {
    char *value = NULL;
    size_t len = 0;
    e = asvault_rec_open(v, &d.recs[i], &value, &len);
    if (e != ASVAULT_OK) {
      printf("error   %s\n", asvault_last_error(v));
      t.errors++;
    } else if (len < ASVAULT_REDACT_MIN) {
      printf("warning %s: the value is %lu bytes, shorter than %d: it is "
             "not redacted from tool output\n",
             d.recs[i].name, (unsigned long)len, ASVAULT_REDACT_MIN);
      t.warnings++;
    } else {
      printf("ok      %s\n", d.recs[i].name);
    }
    asvault_free_secret(value);
  }

out:
  if (d.exists)
    printf("vault: %lu secrets, %d errors, %d warnings\n",
           (unsigned long)d.n, t.errors, t.warnings);
  asvault_doc_free(&d);
  os_mutex_unlock(&v->mu);
  asvault_close(v);
  return t.errors ? 1 : 0;
}

/* ---- entry -------------------------------------------------------------- */

/* argv[0] is the mode name ("vault"); argv[1] the command. */
int asvault_cli_main(int argc, char **argv, const char *vault_dir) {
  const char *cmd;
  if (argc < 2 || !argv) {
    usage(stderr);
    return 2;
  }
  cmd = argv[1];
  if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
    usage(stdout);
    return 0;
  }
  if (!vault_dir || !*vault_dir) {
    fputs("error: no vault directory\n", stderr);
    return 1;
  }
  if (strcmp(cmd, "set") == 0) return cmd_set(argc - 2, argv + 2, vault_dir);
  if (strcmp(cmd, "list") == 0 && argc == 2) return cmd_list(vault_dir);
  if (strcmp(cmd, "rm") == 0 && argc == 3 && argv[2][0] != '-')
    return cmd_rm(vault_dir, argv[2]);
  if (strcmp(cmd, "check") == 0 && argc == 2) return cmd_check(vault_dir);
  if (strcmp(cmd, "set") != 0 && strcmp(cmd, "list") != 0 &&
      strcmp(cmd, "rm") != 0 && strcmp(cmd, "check") != 0)
    fprintf(stderr, "error: unknown command '%s'\n", cmd);
  usage(stderr);
  return 2;
}
