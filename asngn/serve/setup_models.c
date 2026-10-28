/*
 * setup_models.c — the model files of a tier, from the manifest embedded in
 * the executable (scripts/models.manifest.tsv, the one source of truth).
 *
 * Each file is fetched into <name>.part through aslink, resumed with an
 * HTTP Range after an interruption, checked for its pinned size and SHA-256
 * and only then renamed into place. A file already in place is verified,
 * never trusted by name.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <unistd.h>
#else
#include <io.h>
#define isatty _isatty
#endif

#include "aslink.h"
#include "core.h"
#include "setup.h"

static bool field(char **p, char *out, size_t n) {
  char *tab = strchr(*p, '\t');
  size_t len = tab ? (size_t)(tab - *p) : strlen(*p);
  if (len >= n) return false;
  memcpy(out, *p, len);
  out[len] = '\0';
  *p += len + (tab ? 1 : 0);
  return len > 0;
}

static bool in_tier(const char *tiers, const char *tier) {
  size_t n = strlen(tier);
  for (const char *p = tiers; (p = strstr(p, tier)) != NULL; p += n)
    if ((p == tiers || p[-1] == ',') && (p[n] == '\0' || p[n] == ','))
      return true;
  return false;
}

bool setup_manifest_parse(const char *text, const char *tier, setup_model **out,
                          size_t *out_n) {
  setup_model *rows = NULL;
  size_t n = 0, cap = 0;
  char *copy = core_strdup(text ? text : ""), *line, *next;
  *out = NULL;
  *out_n = 0;
  if (!copy) return false;
  for (line = copy; line && *line; line = next) {
    setup_model m;
    char size[32], *p;
    next = strchr(line, '\n');
    if (next) *next++ = '\0';
    core_str_trim(line);
    if (!*line || *line == '#') continue;
    memset(&m, 0, sizeof m);
    p = line;
    if (!field(&p, m.tiers, sizeof m.tiers) || !field(&p, m.name, sizeof m.name) ||
        !field(&p, size, sizeof size) || !field(&p, m.sha256, sizeof m.sha256) ||
        !field(&p, m.url, sizeof m.url) || !field(&p, m.post, sizeof m.post) ||
        strlen(m.sha256) != 64 || strchr(m.name, '/') || strchr(m.name, '\\')) {
      free(rows);
      free(copy);
      return false;
    }
    m.size = strtoull(size, NULL, 10);
    if (tier && !in_tier(m.tiers, tier)) continue;
    if (n == cap) {
      setup_model *grown = realloc(rows, (cap = cap ? cap * 2 : 8) * sizeof *rows);
      if (!grown) {
        free(rows);
        free(copy);
        return false;
      }
      rows = grown;
    }
    rows[n++] = m;
  }
  free(copy);
  *out = rows;
  *out_n = n;
  return true;
}

const char *setup_manifest_tier_of(const char *manifest, const char *config_text) {
  static const char *const order[] = {"plus", "standard", "base"};
  for (size_t t = 0; t < 3; t++) {
    setup_model *m = NULL;
    size_t n = 0, hit = 0;
    if (!setup_manifest_parse(manifest, order[t], &m, &n)) return NULL;
    for (size_t i = 0; i < n; i++)
      if (strstr(config_text, m[i].name)) hit++;
    free(m);
    if (n > 0 && hit == n) return order[t];
  }
  return NULL;
}

static bool verify(const char *path, const setup_model *m) {
  uint64_t size = 0;
  uint8_t hash[32];
  char hex[65];
  if (os_file_size(path, &size) != CORE_OK || size != m->size) return false;
  if (core_sha256_file(path, hash) != CORE_OK) return false;
  core_sha256_hex(hash, 32, hex);
  return strcmp(hex, m->sha256) == 0;
}

static void human(char *out, size_t n, uint64_t bytes) {
  if (bytes >= 1000000000ull) snprintf(out, n, "%.1f GB", (double)bytes / 1e9);
  else snprintf(out, n, "%.0f MB", (double)bytes / 1e6);
}

typedef struct {
  const setup_model *m;
  bool tty;
  bool drawn;
  uint64_t first;   /* bytes in the part file when this attempt started */
  int64_t t0, last; /* monotonic ms                                     */
  int last_quarter;
} progress;

static void on_progress(void *ud, uint64_t have) {
  progress *p = ud;
  int64_t now = os_monotonic_ms();
  int pct = p->m->size ? (int)(have * 100 / p->m->size) : 100;
  char got[24], all[24], rate[24] = "";
  if (!p->t0) {
    p->t0 = now;
    p->first = have;
  }
  if (p->tty) {
    if (now - p->last < 250 && have < p->m->size) return;
    if (now > p->t0 + 500) {
      human(rate, sizeof rate, (have - p->first) * 1000 / (uint64_t)(now - p->t0));
      strcat(rate, "/s");
    }
    human(got, sizeof got, have);
    human(all, sizeof all, p->m->size);
    printf("\r  %-36.36s %3d%%  %s / %s  %-12s", p->m->name, pct, got, all, rate);
    fflush(stdout);
    p->drawn = true;
  } else if (pct / 25 != p->last_quarter) {
    p->last_quarter = pct / 25;
    printf("  %s: %d%%\n", p->m->name, pct);
    fflush(stdout);
  }
  p->last = now;
}

bool setup_models_install(const char *root, const setup_model *m, size_t n) {
  char *dir = os_path_join(root, "models");
  bool all = true;
  if (!dir || os_mkdir_p(dir) != CORE_OK) {
    fprintf(stderr, "  cannot create %s/models\n", root);
    free(dir);
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    char *dest = os_path_join(dir, m[i].name), part[1200], size[24];
    bool done = false, restarted = false;
    if (!dest) {
      all = false;
      break;
    }
    snprintf(part, sizeof part, "%s.part", dest);
    human(size, sizeof size, m[i].size);
    if (strcmp(m[i].post, "-") != 0) {
      printf("  %s: needs a post-processing step: use scripts/fetch-models\n", m[i].name);
      all = false;
      free(dest);
      continue;
    }
    if (os_file_exists(dest)) {
      printf("  %s: checking %s… ", m[i].name, size);
      fflush(stdout);
      if (verify(dest, &m[i])) {
        printf("ok\n");
        free(dest);
        continue;
      }
      printf("wrong checksum, downloading it again\n");
    }
    /* transient failures resume; a bad file starts over once */
    for (int attempt = 0; attempt < 4 && !done; attempt++) {
      progress pg;
      aslink_status st;
      long status = 0;
      aslink_err e;
      memset(&pg, 0, sizeof pg);
      pg.m = &m[i];
      pg.tty = isatty(1) != 0;
      pg.last_quarter = -1;
      e = aslink_http_download(m[i].url, part, m[i].size, NULL, on_progress, &pg, &status, &st);
      if (pg.drawn) printf("\n");
      if (e == ASLINK_OK && verify(part, &m[i])) {
        done = os_rename(part, dest) == CORE_OK;
        printf(done ? "  %s: installed (%s)\n" : "  %s: cannot move it into place\n",
               m[i].name, size);
        break;
      }
      if (e == ASLINK_OK || e == ASLINK_ERR_TOO_LARGE || e == ASLINK_ERR_PROTOCOL) {
        (void)os_remove_file(part);
        if (restarted) {
          printf("  %s: the download does not match its checksum\n", m[i].name);
          break;
        }
        printf("  %s: the partial file is not valid, starting over\n", m[i].name);
        restarted = true;
        continue;
      }
      printf("  %s: download failed: %s%s%s\n", m[i].name, aslink_err_name(e),
             st.message[0] ? " " : "", st.message);
      if (e == ASLINK_ERR_NOT_FOUND || e == ASLINK_ERR_UNSUPPORTED || e == ASLINK_ERR_INVALID)
        break;
      os_sleep_ms(2000 << attempt);
    }
    if (!done) {
      all = false;
      printf("  run asterism setup again to resume\n");
    }
    free(dest);
  }
  free(dir);
  return all;
}
