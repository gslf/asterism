/* project.c — named project folders: the registry (<root>/projects.xcdn),
 * a session's workspace bound to its project, and one turn per project.
 *
 * The registry is read on every lookup (it is small and other frontends of
 * the same root may change it) and rewritten by atomic replace under the
 * coordinator's projects_mu. A project folder is never created inside
 * sessions/ and never deleted: removing a project only unregisters it. */

#include <stdlib.h>
#include <string.h>

#include "asngn_internal.h"
#include "xcdn.h"

struct asngn_project_hold {
  char name[ASNGN_PROJECT_NAME_MAX + 1];
  char slug[65];
};

#if defined(_WIN32)
#define path_ncmp _strnicmp
#else
#define path_ncmp strncmp
#endif

static bool is_sep(char ch) { return ch == '/' || ch == '\\'; }

/* child is parent or lies below it, component-wise. */
static bool path_within(const char *child, const char *parent) {
  size_t n = strlen(parent);
  while (n > 1 && is_sep(parent[n - 1])) n--;
  if (n == 0 || path_ncmp(child, parent, n) != 0) return false;
  return child[n] == '\0' || is_sep(child[n]) || is_sep(parent[n - 1]);
}

static bool path_same(const char *a, const char *b) {
  return path_within(a, b) && path_within(b, a);
}

/* Subcommand words of /project are not names. */
static bool name_valid(const char *name) {
  static const char *const reserved[] = {"new", "rm", "off", "list", "none"};
  size_t i;
  if (name == NULL || !core_slug_valid(name) ||
      strlen(name) > ASNGN_PROJECT_NAME_MAX)
    return false;
  for (i = 0; i < sizeof reserved / sizeof *reserved; i++)
    if (strcmp(name, reserved[i]) == 0) return false;
  return true;
}

static const char *home_dir(void) {
  const char *home = getenv("USERPROFILE");
  if (home == NULL || home[0] == '\0') home = getenv("HOME");
  return home != NULL && home[0] != '\0' ? home : NULL;
}

static char *expand_home(const char *path) {
  const char *home = home_dir();
  if (path[0] != '~' || (path[1] != '\0' && !is_sep(path[1])) || home == NULL)
    return core_strdup(path);
  return path[1] != '\0' ? os_path_join(home, path + 2) : core_strdup(home);
}

/* ── registry ─────────────────────────────────────────────────────────── */

static int by_name(const void *a, const void *b) {
  return strcmp(((const asngn_project_info *)a)->name,
                ((const asngn_project_info *)b)->name);
}

static asngn_err registry_load(asngn_ctx *c, asngn_project_info **out,
                               size_t *out_n) {
  char *path = os_path_join(c->root, "projects.xcdn");
  struct xcdn_document *doc = NULL;
  asngn_project_info *v = NULL;
  size_t n = 0, i;
  asngn_err e;
  *out = NULL;
  *out_n = 0;
  if (path == NULL) return ASNGN_ERR_NOMEM;
  e = asngn_stream_load(c, path, "projects", &doc);
  free(path);
  if (e != ASNGN_OK || doc == NULL) return e;
  {
    xcdn_document_t *d = (xcdn_document_t *)doc;
    const xcdn_value_t *arr =
        d->values_len > 0 ? asngn_xfield(d->values[0]->value, "projects")
                          : NULL;
    if (arr != NULL && arr->type == XCDN_VAL_ARRAY &&
        arr->data.array.len > 0) {
      v = calloc(arr->data.array.len, sizeof *v);
      if (v == NULL) {
        xcdn_document_free(d);
        return ASNGN_ERR_NOMEM;
      }
      for (i = 0; i < arr->data.array.len; i++) {
        const xcdn_value_t *p = arr->data.array.items[i]->value;
        const char *name = asngn_xstr(asngn_xfield(p, "name"));
        const char *dir = asngn_xstr(asngn_xfield(p, "path"));
        int64_t at = 0;
        if (!name_valid(name) || dir == NULL || dir[0] == '\0' ||
            strlen(dir) >= sizeof v[n].path)
          continue;
        (void)asngn_xint(asngn_xfield(p, "created_at"), &at);
        snprintf(v[n].name, sizeof v[n].name, "%s", name);
        snprintf(v[n].path, sizeof v[n].path, "%s", dir);
        v[n].created_at = at;
        n++;
      }
    }
    xcdn_document_free(d);
  }
  if (n == 0) {
    free(v);
    v = NULL;
  }
  *out = v;
  *out_n = n;
  return ASNGN_OK;
}

static asngn_err registry_save(asngn_ctx *c, const asngn_project_info *v,
                               size_t n) {
  xcdn_value_t *obj = xcdn_value_object(), *arr = xcdn_value_array();
  xcdn_node_t *node = NULL;
  core_buf buf;
  char *path = NULL;
  asngn_err e = ASNGN_ERR_NOMEM;
  bool ok = obj != NULL && arr != NULL;
  size_t i;
  core_buf_init(&buf);
  for (i = 0; ok && i < n; i++) {
    xcdn_value_t *p = xcdn_value_object();
    ok = p != NULL;
    ok = ok && asngn_xobj_put(p, "name", xcdn_value_string(v[i].name));
    ok = ok && asngn_xobj_put(p, "path", xcdn_value_string(v[i].path));
    ok = ok && asngn_xobj_put(p, "created_at",
                              xcdn_value_int((int64_t)v[i].created_at));
    if (ok) ok = asngn_xarr_push(arr, p);
    else if (p != NULL) xcdn_value_free(p);
  }
  if (ok) {
    ok = asngn_xobj_put(obj, "projects", arr);
    arr = NULL;
  }
  if (!ok) goto out;
  node = xcdn_node_new(obj);
  if (node == NULL) goto out;
  obj = NULL;
  if (!asngn_xnode_tag(node, "asngn_projects")) goto out;
  e = asngn_xnode_write(node, true, &buf);
  if (e == ASNGN_OK) e = asngn_buf_appendc(&buf, '\n');
  if (e != ASNGN_OK) goto out;
  path = os_path_join(c->root, "projects.xcdn");
  e = path != NULL ? asngn_write_atomic(c, path, buf.data, buf.len)
                   : ASNGN_ERR_NOMEM;
out:
  free(path);
  core_buf_free(&buf);
  if (node != NULL) xcdn_node_free(node);
  if (obj != NULL) xcdn_value_free(obj);
  if (arr != NULL) xcdn_value_free(arr);
  return e;
}

asngn_err asngn_project_lookup(asngn_ctx *c, const char *name,
                               asngn_project_info *out) {
  asngn_project_info *v;
  size_t n, i;
  asngn_err e;
  if (c == NULL || name == NULL || out == NULL) return ASNGN_ERR_INVALID;
  e = registry_load(c, &v, &n);
  if (e != ASNGN_OK) return e;
  e = ASNGN_ERR_NOT_FOUND;
  for (i = 0; i < n; i++)
    if (strcmp(v[i].name, name) == 0) {
      *out = v[i];
      e = ASNGN_OK;
      break;
    }
  free(v);
  return e;
}

/* Why a canonical folder cannot be a project, into why; false if it can. */
static bool folder_refused(asngn_ctx *c, const char *real, char *why,
                           size_t why_size) {
  /* Each listed directory and everything below it. */
  static const char *const system_trees[] = {
#if defined(__APPLE__)
      "/System", "/Library", "/Applications", "/bin", "/sbin", "/usr",
      "/dev", "/cores", "/private/etc",
#elif !defined(_WIN32)
      "/bin", "/boot", "/dev", "/etc", "/lib", "/lib32", "/lib64", "/libx32",
      "/proc", "/run", "/sbin", "/snap", "/sys", "/usr",
#endif
      NULL};
  /* Only the listed directory itself: shared parents of user folders. */
  static const char *const shared_parents[] = {
#if defined(__APPLE__)
      "/Users", "/Volumes", "/private/tmp", "/private/var", "/opt", "/tmp",
#elif !defined(_WIN32)
      "/home", "/mnt", "/media", "/opt", "/srv", "/tmp", "/var",
#endif
      NULL};
#if defined(_WIN32)
  static const char *const win_trees[] = {"SystemRoot", "ProgramFiles",
                                          "ProgramFiles(x86)", "ProgramData"};
#endif
  const char *home = home_dir();
  char *root = asngn_realpath(c->root), *projects, *home_real = NULL;
  bool refused = true;
  size_t i, len = strlen(real);

  if (root == NULL) {
    snprintf(why, why_size, "cannot resolve the asterism root %s", c->root);
    return true;
  }
  projects = os_path_join(root, "projects");
  if (home != NULL) home_real = asngn_realpath(home);
  if (projects == NULL) {
    snprintf(why, why_size, "out of memory");
  } else if ((len == 1 && is_sep(real[0])) ||
             (len <= 3 && len >= 2 && real[1] == ':')) {
    snprintf(why, why_size, "a filesystem root cannot be a project");
  } else if (home_real != NULL && path_same(real, home_real)) {
    snprintf(why, why_size,
             "the home folder itself cannot be a project: pick a folder "
             "inside it");
  } else if (path_within(root, real)) {
    snprintf(why, why_size, "%s contains the asterism root %s", real, root);
  } else if (path_within(real, root) &&
             (!path_within(real, projects) || path_same(real, projects))) {
    snprintf(why, why_size,
             "folders inside the asterism root %s cannot be projects, "
             "except under projects/",
             root);
  } else {
    refused = false;
    for (i = 0; !refused && system_trees[i] != NULL; i++)
      if (path_within(real, system_trees[i])) {
        snprintf(why, why_size, "%s is a system folder", system_trees[i]);
        refused = true;
      }
    for (i = 0; !refused && shared_parents[i] != NULL; i++)
      if (path_same(real, shared_parents[i])) {
        snprintf(why, why_size,
                 "%s holds other folders: pick a folder inside it", real);
        refused = true;
      }
#if defined(_WIN32)
    for (i = 0; !refused && i < sizeof win_trees / sizeof *win_trees; i++) {
      const char *dir = getenv(win_trees[i]);
      if (dir != NULL && dir[0] != '\0' && path_within(real, dir)) {
        snprintf(why, why_size, "%s is a system folder", dir);
        refused = true;
      }
    }
#endif
  }
  free(home_real);
  free(projects);
  free(root);
  return refused;
}

/* ── public registry API ──────────────────────────────────────────────── */

asngn_err asngn_project_add(asngn_ctx *c, const char *name, const char *path,
                            asngn_project_info *out) {
  asngn_project_info *v = NULL, *grown, p;
  size_t n = 0, i;
  char *want = NULL, *real = NULL, why[512];
  asngn_err e;

  if (c == NULL || name == NULL) return ASNGN_ERR_INVALID;
  if (!name_valid(name))
    return asngn_seterr(c, ASNGN_ERR_INVALID,
                        "project name \"%s\": use lowercase letters, digits "
                        "and '-', at most %d characters (not new, rm, off, "
                        "list or none)",
                        name, ASNGN_PROJECT_NAME_MAX);
  if (path == NULL || path[0] == '\0') {
    char *dir = os_path_join(c->root, "projects");
    want = dir != NULL ? os_path_join(dir, name) : NULL;
    free(dir);
    if (want == NULL) return ASNGN_ERR_NOMEM;
    e = os_mkdir_p(want);
    if (e != ASNGN_OK) {
      e = asngn_seterr(c, e, "project %s: cannot create %s", name, want);
      free(want);
      return e;
    }
  } else {
    want = expand_home(path);
    if (want == NULL) return ASNGN_ERR_NOMEM;
  }
  if (!os_path_is_abs(want)) {
    e = asngn_seterr(c, ASNGN_ERR_INVALID,
                     "project folder %s: use an absolute path or ~/...", want);
    free(want);
    return e;
  }
  if (!os_is_dir(want)) {
    e = asngn_seterr(c, ASNGN_ERR_NOT_FOUND, "no such folder: %s", want);
    free(want);
    return e;
  }
  real = asngn_realpath(want);
  free(want);
  if (real == NULL || strlen(real) >= sizeof p.path) {
    free(real);
    return asngn_seterr(c, ASNGN_ERR_INVALID,
                        "project %s: the folder cannot be resolved", name);
  }
  if (folder_refused(c, real, why, sizeof why)) {
    free(real);
    return asngn_seterr(c, ASNGN_ERR_DENIED, "project %s: %s", name, why);
  }
  memset(&p, 0, sizeof p);
  snprintf(p.name, sizeof p.name, "%s", name);
  snprintf(p.path, sizeof p.path, "%s", real);
  p.created_at = (long long)os_now_unix();
  free(real);

  os_mutex_lock(&c->projects_mu);
  e = registry_load(c, &v, &n);
  for (i = 0; e == ASNGN_OK && i < n; i++) {
    if (strcmp(v[i].name, name) == 0)
      e = asngn_seterr(c, ASNGN_ERR_INVALID, "project %s already exists (%s)",
                       name, v[i].path);
    else if (path_within(p.path, v[i].path) || path_within(v[i].path, p.path))
      e = asngn_seterr(c, ASNGN_ERR_DENIED,
                       "project %s: %s overlaps project %s (%s)", name,
                       p.path, v[i].name, v[i].path);
  }
  if (e == ASNGN_OK) {
    grown = realloc(v, (n + 1) * sizeof *v);
    if (grown == NULL) {
      e = ASNGN_ERR_NOMEM;
    } else {
      v = grown;
      v[n++] = p;
      qsort(v, n, sizeof *v, by_name);
      e = registry_save(c, v, n);
    }
  }
  os_mutex_unlock(&c->projects_mu);
  free(v);
  if (e == ASNGN_OK && out != NULL) *out = p;
  return e;
}

asngn_err asngn_project_remove(asngn_ctx *c, const char *name) {
  asngn_project_info *v = NULL;
  size_t n = 0, i;
  asngn_err e;
  if (c == NULL || name == NULL) return ASNGN_ERR_INVALID;
  os_mutex_lock(&c->projects_mu);
  e = registry_load(c, &v, &n);
  if (e == ASNGN_OK) {
    for (i = 0; i < n && strcmp(v[i].name, name) != 0; i++) {
    }
    if (i == n) {
      e = asngn_seterr(c, ASNGN_ERR_NOT_FOUND, "unknown project %s", name);
    } else {
      memmove(&v[i], &v[i + 1], (n - i - 1) * sizeof *v);
      e = registry_save(c, v, n - 1);
    }
  }
  os_mutex_unlock(&c->projects_mu);
  free(v);
  return e;
}

asngn_err asngn_project_get(asngn_ctx *c, const char *name,
                            asngn_project_info *out) {
  asngn_err e;
  if (c == NULL || name == NULL || out == NULL) return ASNGN_ERR_INVALID;
  e = asngn_project_lookup(c, name, out);
  if (e == ASNGN_ERR_NOT_FOUND)
    return asngn_seterr(c, e, "unknown project %s", name);
  return e;
}

asngn_err asngn_projects_list(asngn_ctx *c, asngn_project_info **out,
                              size_t *out_n) {
  asngn_err e;
  if (c == NULL || out == NULL || out_n == NULL) return ASNGN_ERR_INVALID;
  e = registry_load(c, out, out_n);
  if (e == ASNGN_OK && *out_n > 1) qsort(*out, *out_n, sizeof **out, by_name);
  return e;
}

/* ── sessions ─────────────────────────────────────────────────────────── */

/* Approvals and the code index are bound to the old folder. */
static void workspace_moved(asngn_session *s) {
  size_t i;
  for (i = 0; i < s->allow_n; i++) free(s->allow[i]);
  free(s->allow);
  s->allow = NULL;
  s->allow_n = 0;
  asngn_code_index_free(s->code_index);
  s->code_index = NULL;
}

asngn_err asngn_session_project_sync(asngn_ctx *c, asngn_session *s) {
  asngn_project_info p;
  asngn_workspace_info w;
  char *dir = NULL, *real = NULL;
  asngn_err e;
  memset(&w, 0, sizeof w);
  if (c == NULL || s == NULL) return ASNGN_ERR_INVALID;
  if (!c->session_workspaces) return ASNGN_OK;
  if (s->project != NULL && asngn_project_lookup(c, s->project, &p) == ASNGN_OK) {
    if (!os_is_dir(p.path))
      return asngn_seterr(c, ASNGN_ERR_NOT_FOUND,
                          "project %s: its folder %s is missing (restore it, "
                          "or remove the project)",
                          p.name, p.path);
    if (strcmp(s->workspace.canonical_root, p.path) == 0) return ASNGN_OK;
    e = asngn_workspace_info_init(c, p.path, &w);
  } else {
    dir = os_path_join(s->dir, "workspace");
    if (dir == NULL) return ASNGN_ERR_NOMEM;
    e = os_mkdir_p(dir);
    real = e == ASNGN_OK ? asngn_realpath(dir) : NULL;
    if (real != NULL && strcmp(s->workspace.canonical_root, real) == 0) {
      free(real);
      free(dir);
      return ASNGN_OK;
    }
    free(real);
    if (e == ASNGN_OK) e = asngn_workspace_info_init(c, dir, &w);
    free(dir);
  }
  if (e != ASNGN_OK) return e;
  s->workspace = w;
  s->workspace_loaded = true;
  workspace_moved(s);
  return ASNGN_OK;
}

/* ── one turn per project ─────────────────────────────────────────────── */

asngn_err asngn_project_hold(asngn_ctx *c, const char *name,
                             const char *slug) {
  struct asngn_project_hold *grown;
  char other[65] = "";
  size_t i;
  os_mutex_lock(&c->projects_mu);
  for (i = 0; i < c->project_holds_n; i++)
    if (strcmp(c->project_holds[i].name, name) == 0) {
      snprintf(other, sizeof other, "%s", c->project_holds[i].slug);
      break;
    }
  grown = other[0] == '\0'
              ? realloc(c->project_holds,
                        (c->project_holds_n + 1) * sizeof *grown)
              : NULL;
  if (grown != NULL) {
    c->project_holds = grown;
    snprintf(grown[c->project_holds_n].name, sizeof grown->name, "%s", name);
    snprintf(grown[c->project_holds_n].slug, sizeof grown->slug, "%s", slug);
    c->project_holds_n++;
  }
  os_mutex_unlock(&c->projects_mu);
  if (other[0] != '\0')
    return asngn_seterr(c, ASNGN_ERR_BUSY,
                        "project %s: a turn is running in session %s", name,
                        other);
  return grown != NULL ? ASNGN_OK : ASNGN_ERR_NOMEM;
}

int asngn_project_busy(asngn_ctx *c, const char *name, char *slug,
                       size_t slug_size) {
  int busy = 0;
  size_t i;
  if (c == NULL || name == NULL) return 0;
  os_mutex_lock(&c->projects_mu);
  for (i = 0; i < c->project_holds_n && !busy; i++)
    if (strcmp(c->project_holds[i].name, name) == 0) {
      if (slug != NULL && slug_size > 0)
        snprintf(slug, slug_size, "%s", c->project_holds[i].slug);
      busy = 1;
    }
  os_mutex_unlock(&c->projects_mu);
  return busy;
}

void asngn_project_release(asngn_ctx *c, const char *name) {
  size_t i;
  if (c == NULL || name == NULL || name[0] == '\0') return;
  os_mutex_lock(&c->projects_mu);
  for (i = 0; i < c->project_holds_n; i++)
    if (strcmp(c->project_holds[i].name, name) == 0) {
      c->project_holds[i] = c->project_holds[--c->project_holds_n];
      break;
    }
  os_mutex_unlock(&c->projects_mu);
}

void asngn_project_holds_free(asngn_ctx *c) {
  free(c->project_holds);
  c->project_holds = NULL;
  c->project_holds_n = 0;
}
