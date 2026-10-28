/*
 * tool_cli.c — "asterism vault" built standalone for the CLI test:
 *   tool_cli <vault-dir> vault <command>...
 * plus two fixture commands that print what the host would pass on, so the
 * test can check stored values without a CLI that prints them:
 *   tool_cli <vault-dir> --resolve <tool> <text> [<domain>]
 *   tool_cli <vault-dir> --redact <text>
 */
#include <stdio.h>
#include <string.h>

#include "asvault.h"

static int fixture(const char *dir, int argc, char **argv) {
  asvault *v = NULL;
  char *out = NULL;
  asvault_err e = asvault_open(dir, &v);
  if (e == ASVAULT_OK && (argc == 3 || argc == 4) &&
      strcmp(argv[0], "--resolve") == 0)
    e = asvault_resolve(v, argv[2], argv[1], argc == 4 ? argv[3] : NULL, &out);
  else if (e == ASVAULT_OK && argc == 2 && strcmp(argv[0], "--redact") == 0)
    e = asvault_redact(v, argv[1], &out);
  else if (e == ASVAULT_OK)
    e = ASVAULT_ERR_INVALID;
  if (e == ASVAULT_OK) fputs(out, stdout);
  else fprintf(stderr, "error: %s %s\n", asvault_err_name(e),
               asvault_last_error(v));
  asvault_free_secret(out);
  asvault_close(v);
  return e == ASVAULT_OK ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fputs("usage: tool_cli <vault-dir> vault <command>...\n", stderr);
    return 2;
  }
  if (argv[2][0] == '-' && argv[2][1] == '-')
    return fixture(argv[1], argc - 2, argv + 2);
  return asvault_cli_main(argc - 2, argv + 2, argv[1]);
}
