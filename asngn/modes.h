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

#endif /* ASNGN_MODES_H */
