/*
 * modes.h — the modes of the asterism executable (main.c dispatches).
 * Each entry receives argv starting at its mode name and returns the exit
 * status.
 */

#ifndef ASNGN_MODES_H
#define ASNGN_MODES_H

#include <stdio.h>

int  asngn_tui_main(int argc, char **argv); /* default: TUI or --once */
void asngn_tui_usage(FILE *out);
int  asngn_mcp_main(int argc, char **argv);
int  asngn_acp_main(int argc, char **argv); /* POSIX only */
int  asngn_serve_main(int argc, char **argv); /* always-on daemon */
void asngn_serve_usage(FILE *out);
int  asngn_setup_main(int argc, char **argv); /* guided install */
/* asterism start | stop | status: the background agent (argv[0] is the verb). */
int  asngn_service_main(int argc, char **argv);
/* The serve-side checks of doctor: tools, token, pairing, service. */
void asngn_setup_report(const char *root, FILE *out);
int  asngn_login_main(int argc, char **argv); /* login|logout chatgpt */
int  asngn_vault_main(int argc, char **argv); /* secrets for tools */

#endif /* ASNGN_MODES_H */
