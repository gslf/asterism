/* Skills: procedures that worked, kept per star under <root>/skills/<name>/.
 *
 * SKILL.md follows the Agent Skills layout (front matter name and
 * description, then instructions) so other agents can read it; skill.xcdn
 * holds the daemon's bookkeeping. Two sources:
 *   - owner: the owner saved a finished task with /skill save;
 *   - delegate: a delegated agent (Codex, Claude Code) solved a task and the
 *     daemon kept what it did as a playbook for the local model.
 * A skill starts provisional and becomes trusted after two successful uses.
 * A delegate playbook stays untrusted text until the owner saves it: its hint
 * taints the turn, so it can advise but never authorize. */
#include "daemon.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xcdn.h"

#define SKILLS_MAX 64
#define TRUST_AFTER 2

typedef struct {
  char name[65];
  char status[16];       /* provisional | trusted */
  char source[16];       /* owner | delegate      */
  int64_t uses, successes, failures;
  char *description, *body;
} skill;

static void skill_free(skill *s) {
  free(s->description);
  free(s->body);
}

static void skills_dir(serve_star *star, char *out, size_t n) {
  snprintf(out, n, "%s/skills", star->root);
}

static int64_t xint(const xcdn_value_t *obj, const char *key) {
  xcdn_node_t *n = xcdn_object_get(obj, key);
  return n && n->value && n->value->type == XCDN_VAL_INT ? xcdn_value_as_int(n->value) : 0;
}

static void xstr(const xcdn_value_t *obj, const char *key, char *out, size_t size) {
  xcdn_node_t *n = xcdn_object_get(obj, key);
  out[0] = 0;
  if (n && n->value && n->value->type == XCDN_VAL_STRING)
    snprintf(out, size, "%s", xcdn_value_as_string(n->value));
}

static bool skill_load(serve_star *star, const char *name, skill *out) {
  char dir[1300], path[1400], *text = NULL;
  size_t len = 0;
  xcdn_error_t xe;
  memset(out, 0, sizeof *out);
  if (!core_slug_valid(name)) return false;
  skills_dir(star, dir, sizeof dir);
  snprintf(path, sizeof path, "%s/%s/skill.xcdn", dir, name);
  if (os_read_file(path, &text, &len) != CORE_OK) return false;
  xcdn_document_t *doc = xcdn_parse_str(text, len, &xe);
  free(text);
  const xcdn_value_t *obj = doc && doc->values_len ? doc->values[0]->value : NULL;
  if (!obj || obj->type != XCDN_VAL_OBJECT) {
    if (doc) xcdn_document_free(doc);
    return false;
  }
  snprintf(out->name, sizeof out->name, "%s", name);
  xstr(obj, "status", out->status, sizeof out->status);
  xstr(obj, "source", out->source, sizeof out->source);
  out->uses = xint(obj, "uses");
  out->successes = xint(obj, "successes");
  out->failures = xint(obj, "failures");
  xcdn_document_free(doc);
  snprintf(path, sizeof path, "%s/%s/SKILL.md", dir, name);
  if (os_read_file(path, &out->body, &len) != CORE_OK) out->body = core_strdup("");
  /* description: the front matter line */
  const char *d = out->body ? strstr(out->body, "\ndescription: ") : NULL;
  if (d) {
    d += 14;
    const char *e = strchr(d, '\n');
    out->description = core_strndup(d, e ? (size_t)(e - d) : strlen(d));
  } else out->description = core_strdup("");
  return out->body && out->description;
}

static bool write_file(const char *path, const char *text) {
  char tmp[1500];
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  if (os_write_file(tmp, text, strlen(text)) != CORE_OK) return false;
  if (os_file_replace(tmp, path) != CORE_OK) {
    os_remove_file(tmp);
    return false;
  }
  return true;
}

static bool meta_save(serve_star *star, const skill *s) {
  char dir[1300], path[1400], body[512];
  skills_dir(star, dir, sizeof dir);
  snprintf(path, sizeof path, "%s/%s/skill.xcdn", dir, s->name);
  snprintf(body, sizeof body,
           "#skill { status: \"%s\", source: \"%s\", uses: %lld, successes: %lld, "
           "failures: %lld, updated_at: %lld }\n",
           s->status, s->source, (long long)s->uses, (long long)s->successes,
           (long long)s->failures, (long long)os_now_unix());
  return write_file(path, body);
}

/* One line, no front-matter breakers, bounded. */
static char *one_line(const char *s, size_t max) {
  core_buf b;
  core_buf_init(&b);
  for (size_t i = 0; s && s[i] && b.len < max; i++)
    core_buf_appendc(&b, s[i] == '\n' || s[i] == '\r' ? ' ' : s[i]);
  while (b.len && (b.data[b.len - 1] & 0xC0) == 0x80) b.len--, b.data[b.len] = 0;
  return core_buf_detach(&b);
}

static bool skill_write(serve_star *star, const char *name, const char *source,
                        const char *request, const char *steps, const char *answer) {
  char dir[1300], path[1400];
  core_buf b;
  skill meta;
  bool ok;
  skills_dir(star, dir, sizeof dir);
  snprintf(path, sizeof path, "%s/%s", dir, name);
  if (os_mkdir_p(path) != CORE_OK) return false;
  char *desc = one_line(request, 200);
  core_buf_init(&b);
  core_buf_printf(&b, "---\nname: %s\ndescription: %s\n---\n\n# %s\n\n## When to use\n\n%s\n",
                  name, desc ? desc : "", name, request ? request : "");
  if (steps && *steps) {
    core_buf_appends(&b, "\n## Steps that worked\n\n");
    int i = 1;
    for (const char *p = steps; *p;) {
      const char *e = strchr(p, ' ');
      size_t n = e ? (size_t)(e - p) : strlen(p);
      core_buf_printf(&b, "%d. %.*s\n", i++, (int)n, p);
      p += n;
      while (*p == ' ') p++;
    }
  }
  if (answer && *answer) {
    size_t n = strlen(answer);
    if (n > 1500) n = 1500;
    while (n && !core_utf8_valid(answer, n)) n--;
    core_buf_appends(&b, "\n## Outcome\n\n");
    core_buf_append(&b, answer, n);
    core_buf_appendc(&b, '\n');
  }
  snprintf(path, sizeof path, "%s/%s/SKILL.md", dir, name);
  ok = write_file(path, b.data);
  core_buf_free(&b);
  free(desc);
  memset(&meta, 0, sizeof meta);
  snprintf(meta.name, sizeof meta.name, "%s", name);
  snprintf(meta.status, sizeof meta.status, "provisional");
  snprintf(meta.source, sizeof meta.source, "%s", source);
  return ok && meta_save(star, &meta);
}

/* ── matching ─────────────────────────────────────────────────────────── */

/* Lowercase ASCII words of at least four letters, deduplicated. */
static size_t words(const char *s, char out[][24], size_t cap) {
  size_t n = 0;
  while (s && *s && n < cap) {
    while (*s && !isalnum((unsigned char)*s)) s++;
    char w[24];
    size_t len = 0;
    while (*s && (isalnum((unsigned char)*s) || ((unsigned char)*s & 0x80))) {
      if (len + 1 < sizeof w) w[len++] = (char)tolower((unsigned char)*s);
      s++;
    }
    w[len] = 0;
    if (len < 4) continue;
    bool dup = false;
    for (size_t i = 0; i < n && !dup; i++) dup = !strcmp(out[i], w);
    if (!dup) memcpy(out[n++], w, sizeof w);
  }
  return n;
}

char *serve_skills_hint(serve_star *star, const char *request, char name[65],
                        bool *untrusted) {
  char dir[1300], req[64][24], cand[96][24];
  char **names = NULL;
  size_t n = 0, nr;
  double best = 0;
  skill pick;
  bool have = false;
  name[0] = 0;
  *untrusted = false;
  skills_dir(star, dir, sizeof dir);
  if (os_list_dirs(dir, &names, &n) != CORE_OK) return NULL;
  nr = words(request, req, 64);
  for (size_t i = 0; i < n && i < SKILLS_MAX && nr; i++) {
    skill s;
    if (!skill_load(star, names[i], &s)) continue;
    size_t nc = words(s.description, cand, 96), hits = 0;
    for (size_t a = 0; a < nr; a++)
      for (size_t b = 0; b < nc; b++)
        if (!strcmp(req[a], cand[b])) hits++;
    double score = nc ? (double)hits / sqrt((double)nr * (double)nc) : 0;
    if (hits >= 2 && score >= 0.35 && score > best && strcmp(s.status, "retired")) {
      if (have) skill_free(&pick);
      pick = s;
      best = score;
      have = true;
    } else skill_free(&s);
  }
  os_free_names(names, n);
  if (!have) return NULL;
  bool trusted = !strcmp(pick.status, "trusted");
  *untrusted = !strcmp(pick.source, "delegate");
  core_buf b;
  core_buf_init(&b);
  core_buf_printf(&b,
                  "[Known procedure \"%s\" (%s%s). Use it as guidance and verify each step:\n%s]",
                  pick.name, trusted ? "trusted" : "provisional",
                  *untrusted ? ", written by a delegated agent" : "", pick.body);
  snprintf(name, 65, "%s", pick.name);
  pick.uses++;
  (void)meta_save(star, &pick);
  skill_free(&pick);
  return core_buf_detach(&b);
}

void serve_skills_outcome(struct serve_daemon *d, serve_star *star, const char *name, bool ok) {
  skill s;
  (void)d;
  if (!skill_load(star, name, &s)) return;
  if (ok) s.successes++;
  else s.failures++;
  /* Owner-saved skills earn trust by working; delegate playbooks need the
   * owner to save them first. Repeated failures retire a skill. */
  if (!strcmp(s.source, "owner") && s.successes >= TRUST_AFTER && s.failures < s.successes)
    snprintf(s.status, sizeof s.status, "trusted");
  if (s.failures >= 3 && s.failures > s.successes) snprintf(s.status, sizeof s.status, "retired");
  (void)meta_save(star, &s);
  skill_free(&s);
}

void serve_skills_playbook(struct serve_daemon *d, serve_star *star, const char *request,
                           const char *steps, const char *answer) {
  char base[ASTIME_NAME_MAX], name[65];
  skill existing;
  (void)d;
  astime_slug_from_text(request, base);
  snprintf(name, sizeof name, "auto-%.58s", base);
  /* Never overwrite what the owner saved. */
  if (skill_load(star, name, &existing)) {
    bool owner = !strcmp(existing.source, "owner");
    skill_free(&existing);
    if (owner) return;
  }
  (void)skill_write(star, name, "delegate", request, steps, answer);
}

/* ── /skill ───────────────────────────────────────────────────────────── */

void serve_skills_command(struct serve_daemon *d, serve_star *star, aslink_ctx *bot,
                          int64_t chat, int64_t thread, serve_session *sess,
                          const char *args) {
  char dir[1300];
  bool save = !strncmp(args, "save ", 5);
  bool rm = !strncmp(args, "rm ", 3);
  if (save || rm) {
    const char *raw = strchr(args, ' ') + 1;
    char name[ASTIME_NAME_MAX];
    astime_slug_from_text(raw, name);
    if (rm) {
      char path[1400], file[1500];
      skills_dir(star, dir, sizeof dir);
      snprintf(path, sizeof path, "%s/%s", dir, name);
      snprintf(file, sizeof file, "%s/SKILL.md", path);
      os_remove_file(file);
      snprintf(file, sizeof file, "%s/skill.xcdn", path);
      os_remove_file(file);
      os_remove_dir(path);
      serve_sendf(star, bot, chat, thread, false, serve_s(S_SKILL_REMOVED), name);
      return;
    }
    char *request = NULL, *answer = NULL, steps[512] = "";
    os_mutex_lock(&d->mu);
    if (sess && sess->last_request) {
      request = core_strdup(sess->last_request);
      answer = core_strdup(sess->last_answer ? sess->last_answer : "");
      snprintf(steps, sizeof steps, "%s", sess->steps);
    }
    os_mutex_unlock(&d->mu);
    if (!request) {
      serve_send(star, bot, chat, thread, serve_s(S_SKILL_NONE), false);
    } else if (skill_write(star, name, "owner", request, steps, answer)) {
      serve_sendf(star, bot, chat, thread, false, serve_s(S_SKILL_SAVED), name);
    } else {
      serve_sendf(star, bot, chat, thread, false, serve_s(S_ERROR), "skill");
    }
    free(request);
    free(answer);
    return;
  }
  char **names = NULL;
  size_t n = 0;
  core_buf b;
  skills_dir(star, dir, sizeof dir);
  core_buf_init(&b);
  if (os_list_dirs(dir, &names, &n) == CORE_OK)
    for (size_t i = 0; i < n; i++) {
      skill s;
      if (!skill_load(star, names[i], &s)) continue;
      core_buf_printf(&b, "• %s — %s, %s, %lld/%lld ok — %s\n", s.name, s.status, s.source,
                      (long long)s.successes, (long long)s.uses, s.description);
      skill_free(&s);
    }
  os_free_names(names, n);
  serve_send(star, bot, chat, thread, b.len ? b.data : serve_s(S_SKILL_NONE), false);
  core_buf_free(&b);
}
