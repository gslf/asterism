/*
 * setup.h — the pieces of "asterism setup" (see setup.c): verified model
 * downloads (setup_models.c) and the user service (setup_service.c).
 */

#ifndef ASNGN_SETUP_H
#define ASNGN_SETUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ── models ──────────────────────────────────────────────────────────── */

/* One row of scripts/models.manifest.tsv. */
typedef struct {
  char tiers[64];
  char name[128];
  uint64_t size;
  char sha256[65];
  char url[512];
  char post[128];   /* "-" or a post-processing step (legacy tier only) */
} setup_model;

/* The rows of tier (NULL = every row); malloc'd, free() it. false on a
 * malformed row. */
bool setup_manifest_parse(const char *text, const char *tier, setup_model **out,
                          size_t *out_n);
/* The tier whose every model file name appears in config_text, most
 * specific first (plus, standard, base); NULL when none matches. */
const char *setup_manifest_tier_of(const char *manifest, const char *config_text);
/* Downloads what is missing into <root>/models, resuming .part files, and
 * verifies size and SHA-256 of every file. Prints one line per model. */
bool setup_models_install(const char *root, const setup_model *m, size_t n);

/* ── processes and the user service ──────────────────────────────────── */

/* Runs argv (argv[0] looked up in PATH) and waits. Exit status, or -1 when
 * it cannot start. quiet discards its output. */
int  setup_run(const char *const *argv, bool quiet);
bool setup_in_path(const char *program);
/* Full path of program found in PATH into out; false when absent. */
bool setup_which(const char *program, char *out, size_t n);
/* Absolute path of this executable. */
bool setup_self_path(char *out, size_t n, const char *argv0);

typedef enum {
  SETUP_SERVICE_NONE,     /* nothing to install into (no systemd --user)  */
  SETUP_SERVICE_SYSTEMD,  /* Linux: systemd user unit                     */
  SETUP_SERVICE_LAUNCHD,  /* macOS: LaunchAgent                           */
  SETUP_SERVICE_RUNKEY    /* Windows: HKCU Run entry plus a background run */
} setup_service_kind;

setup_service_kind setup_service_kind_here(void);
const char *setup_service_where(void);  /* unit, plist or registry value */
bool setup_service_installed(void);     /* for any engine root            */
/* Installed for this root, so asterism start can run it. */
bool setup_service_ready(const char *root);
/* It also starts when the user logs in (otherwise only by asterism start). */
bool setup_service_autostart(const char *root);
bool setup_service_active(const char *root);
bool setup_service_start(const char *exe, const char *root);
bool setup_service_stop(const char *root);
/* Writes the service for exe and root. With autostart it starts at every
 * login and (re)starts now; without, asterism start runs it, and it is only
 * restarted if it was running. */
bool setup_service_install(const char *exe, const char *root, bool autostart,
                           bool interactive);
bool setup_service_remove(const char *root);

#endif /* ASNGN_SETUP_H */
