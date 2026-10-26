/*
 * main.c — the asterism executable. One binary; the first argument picks
 * the mode:
 *
 *   asterism [options]          terminal UI, or one turn with --once
 *   asterism mcp [options]      MCP server on stdio
 *   asterism acp [options]      editor host on stdio (POSIX)
 *   asterism doctor [options]   configuration and model check
 *   asterism tools <command>    tool-package commands for authors
 *
 * "jail" is internal: the strict sandbox re-executes this binary to
 * confine a tool. It is dispatched first, before anything is initialized.
 */

#include <stdio.h>
#include <string.h>

#include "asngn.h"
#include "astools.h"
#include "modes.h"

static void usage(FILE *out) {
  fputs("asterism — agent harness for real workflows\n\n"
        "usage: asterism [options]              terminal UI\n"
        "       asterism --once <message|-> [options]\n"
        "       asterism mcp [options]          MCP server on stdio\n"
        "       asterism acp --workspace <dir>  editor host on stdio\n"
        "       asterism doctor [options]       check configuration and models\n"
        "       asterism tools <command> ...    check, catalog, grammar, approve, invoke\n"
        "       asterism --version\n\n"
        "The engine modes take --root <dir> (default ~/asterism) and\n"
        "--config <file>; \"asterism <mode> --help\" describes the rest.\n\n",
        out);
  asngn_tui_usage(out);
}

/* "doctor [--root <dir>] [--config <file>]": no inference, no state. */
static int doctor_main(int argc, char **argv) {
  asngn_open_params params;
  char *report = NULL;
  asngn_err e;
  int i;
  memset(&params, 0, sizeof params);
  for (i = 1; i < argc; i++) {
    const char **slot = !strcmp(argv[i], "--root")     ? &params.engine_root
                        : !strcmp(argv[i], "--config") ? &params.config_path
                                                       : NULL;
    if (!slot || i + 1 >= argc) {
      fputs("usage: asterism doctor [--root <dir>] [--config <file>]\n", stderr);
      return 2;
    }
    *slot = argv[++i];
  }
  e = asngn_diagnose(&params, &report);
  if (report) fputs(report, stdout);
  asngn_free(report);
  return e == ASNGN_OK ? 0 : 1;
}

int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "";
#if !defined(_WIN32)
  if (strcmp(mode, "jail") == 0) return astools_jail_main(argc - 1, argv + 1);
#endif
  if (strcmp(mode, "mcp") == 0) return asngn_mcp_main(argc - 1, argv + 1);
  if (strcmp(mode, "acp") == 0) {
#if defined(ASTERISM_ACP)
    return asngn_acp_main(argc - 1, argv + 1);
#else
    fputs("asterism: acp needs a threaded POSIX build\n", stderr);
    return 2;
#endif
  }
  if (strcmp(mode, "doctor") == 0) return doctor_main(argc - 1, argv + 1);
  if (strcmp(mode, "tools") == 0) return astools_cli_main(argc - 1, argv + 1);
  if (strcmp(mode, "--version") == 0) {
    printf("asterism %s\n", asngn_version());
    return 0;
  }
  if (strcmp(mode, "--help") == 0 || strcmp(mode, "help") == 0) {
    usage(stdout);
    return 0;
  }
  return asngn_tui_main(argc, argv);
}
