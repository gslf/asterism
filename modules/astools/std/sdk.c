/*
 * sdk.c — shared plumbing of the astools-std multi-tool binary.
 *
 * Owns the wire protocol: reads the single #tool_request from stdin and
 * emits the single #tool_response on stdout. All input is treated as
 * hostile: sizes are overflow-checked, every allocation is checked, and
 * any malformed request yields an astools/protocol error response.
 *
 * Links xcdn only (never libastools) so astools-std stays standalone.
 */

#include "sdk.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- field helpers ------------------------------------------------------ */

static const xcdn_value_t *obj_field(const xcdn_value_t *obj, const char *k) {
  const xcdn_node_t *n = xcdn_object_get(obj, k);
  return n ? n->value : NULL;
}

static const char *obj_field_str(const xcdn_value_t *obj, const char *k) {
  const xcdn_value_t *v = obj_field(obj, k);
  if (v && v->type == XCDN_VAL_STRING) return v->data.string;
  return NULL;
}

/* ---- result-building helpers -------------------------------------------- */

int astd_set_val(xcdn_value_t *obj, const char *k, xcdn_value_t *v) {
  xcdn_node_t *node;
  if (!obj || obj->type != XCDN_VAL_OBJECT || !k || !v) {
    xcdn_value_free(v);
    return -1;
  }
  node = xcdn_node_new(v);
  if (!node) {
    xcdn_value_free(v);
    return -1;
  }
  xcdn_object_set(obj, k, node);
  /* xcdn mutators are void; verify the store actually happened. */
  if (xcdn_object_get(obj, k) != node) {
    xcdn_node_free(node);
    return -1;
  }
  return 0;
}

int astd_set_str(xcdn_value_t *obj, const char *k, const char *v) {
  xcdn_value_t *val = xcdn_value_string(v ? v : "");
  if (!val || !val->data.string) {
    xcdn_value_free(val);
    return -1;
  }
  return astd_set_val(obj, k, val);
}

int astd_set_int(xcdn_value_t *obj, const char *k, int64_t v) {
  xcdn_value_t *val = xcdn_value_int(v);
  if (!val) return -1;
  return astd_set_val(obj, k, val);
}

int astd_set_bool(xcdn_value_t *obj, const char *k, int v) {
  xcdn_value_t *val = xcdn_value_bool(v != 0);
  if (!val) return -1;
  return astd_set_val(obj, k, val);
}

int astd_arr_push_val(xcdn_value_t *arr, xcdn_value_t *v) {
  xcdn_node_t *node;
  size_t before;
  if (!arr || arr->type != XCDN_VAL_ARRAY || !v) {
    xcdn_value_free(v);
    return -1;
  }
  node = xcdn_node_new(v);
  if (!node) {
    xcdn_value_free(v);
    return -1;
  }
  before = xcdn_array_len(arr);
  xcdn_array_push(arr, node);
  if (xcdn_array_len(arr) != before + 1) {
    xcdn_node_free(node);
    return -1;
  }
  return 0;
}

/* ---- response emission --------------------------------------------------- */

/* Last-resort response when even building the AST failed. Static text so
 * it cannot itself fail to build. */
static void emit_fallback(void) {
  static const char k_fb[] =
      "#tool_response {protocol: 1, ok: false, error: "
      "{code: \"astools/protocol\", "
      "message: \"internal error while building response\", "
      "retriable: false}}\n";
  (void)fputs(k_fb, stdout);
  (void)fflush(stdout);
}

/* Build and print the #tool_response. Takes ownership of payload (used
 * only when ok). Never returns an error: on failure it prints the static
 * fallback response instead. */
static void emit_response(const astd_req *r, int ok, xcdn_value_t *payload,
                          const char *code, const char *msg) {
  xcdn_document_t *doc = NULL;
  xcdn_node_t *node = NULL;
  xcdn_value_t *obj = NULL;
  char *text = NULL;

  obj = xcdn_value_object();
  if (!obj) goto fail;
  if (astd_set_int(obj, "protocol", 1) != 0) goto fail;
  if (r && r->invocation_id) {
    xcdn_value_t *u = xcdn_value_uuid(r->invocation_id);
    if (!u || !u->data.string) {
      xcdn_value_free(u);
      goto fail;
    }
    if (astd_set_val(obj, "invocation_id", u) != 0) goto fail;
  }
  if (astd_set_bool(obj, "ok", ok) != 0) goto fail;
  if (ok) {
    if (!payload) goto fail;
    if (astd_set_val(obj, "result", payload) != 0) {
      payload = NULL; /* consumed (and freed) by astd_set_val */
      goto fail;
    }
    payload = NULL;
  } else {
    xcdn_value_t *e = xcdn_value_object();
    if (!e) goto fail;
    if (astd_set_str(e, "code", code ? code : "astools/protocol") != 0 ||
        astd_set_str(e, "message", msg ? msg : "") != 0 ||
        astd_set_bool(e, "retriable", 0) != 0) {
      xcdn_value_free(e);
      goto fail;
    }
    if (astd_set_val(obj, "error", e) != 0) goto fail;
  }

  node = xcdn_node_new(obj);
  if (!node) goto fail;
  obj = NULL; /* owned by node */
  xcdn_node_add_tag(node, "tool_response");
  if (!xcdn_node_has_tag(node, "tool_response")) goto fail;

  doc = xcdn_document_new();
  if (!doc) goto fail;
  xcdn_document_push_value(doc, node);
  if (doc->values_len != 1) goto fail;
  node = NULL; /* owned by doc */

  text = xcdn_to_string_compact(doc);
  if (!text) goto fail;
  (void)fputs(text, stdout);
  (void)fputc('\n', stdout);
  (void)fflush(stdout);
  free(text);
  xcdn_document_free(doc);
  return;

fail:
  xcdn_value_free(payload);
  xcdn_value_free(obj);
  xcdn_node_free(node);
  xcdn_document_free(doc);
  free(text);
  emit_fallback();
}

void astd_ok(const astd_req *r, xcdn_value_t *result) {
  if (!result) {
    emit_response(r, 0, NULL, "astools/protocol", "tool produced no result");
    return;
  }
  emit_response(r, 1, result, NULL, NULL);
}

void astd_fail(const astd_req *r, const char *code, const char *fmt, ...) {
  char msg[1024];
  msg[0] = '\0';
  if (fmt) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
  }
  emit_response(r, 0, NULL, code, msg);
}

/* ---- request parsing ----------------------------------------------------- */

/* Emit a protocol error (invocation_id included when already extracted),
 * release the partially built request, and return 1. */
static int req_error(astd_req *r, const char *fmt, ...) {
  char msg[512];
  va_list ap;
  msg[0] = '\0';
  va_start(ap, fmt);
  (void)vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  astd_fail(r, "astools/protocol", "%s", msg);
  if (r) {
    xcdn_document_free(r->doc);
    memset(r, 0, sizeof *r);
  }
  return 1;
}

int astd_req_read(astd_req *r) {
  char *buf = NULL;
  size_t len = 0, cap = 0;
  const size_t chunk = 65536;
  xcdn_error_t err;
  const xcdn_node_t *node = NULL;
  const xcdn_value_t *obj;
  const xcdn_value_t *v;
  size_t i;

  if (!r) return 1;
  memset(r, 0, sizeof *r);

  for (;;) {
    size_t n;
    if (cap - len < chunk + 1) {
      size_t ncap = cap ? cap : chunk + 1;
      char *p;
      while (ncap - len < chunk + 1) {
        if (ncap > SIZE_MAX / 2) {
          free(buf);
          return req_error(r, "request too large");
        }
        ncap *= 2;
      }
      p = realloc(buf, ncap);
      if (!p) {
        free(buf);
        return req_error(r, "out of memory while reading request");
      }
      buf = p;
      cap = ncap;
    }
    n = fread(buf + len, 1, chunk, stdin);
    len += n;
    if (n < chunk) {
      if (ferror(stdin)) {
        free(buf);
        return req_error(r, "error reading request from stdin");
      }
      break;
    }
  }
  buf[len] = '\0';

  r->doc = xcdn_parse_str(buf, len, &err);
  free(buf);
  if (!r->doc)
    return req_error(r, "request parse error: %s", err.message);

  for (i = 0; i < r->doc->values_len; i++) {
    const xcdn_node_t *cand = xcdn_document_get(r->doc, i);
    if (cand && xcdn_node_has_tag(cand, "tool_request")) {
      node = cand;
      break;
    }
  }
  if (!node) return req_error(r, "no #tool_request node in input");
  obj = node->value;
  if (!obj || obj->type != XCDN_VAL_OBJECT)
    return req_error(r, "#tool_request is not an object");

  /* invocation_id first so later error responses can carry it */
  v = obj_field(obj, "invocation_id");
  if (v && (v->type == XCDN_VAL_UUID || v->type == XCDN_VAL_STRING) &&
      v->data.string)
    r->invocation_id = v->data.string;

  v = obj_field(obj, "protocol");
  if (!v || v->type != XCDN_VAL_INT)
    return req_error(r, "missing or non-integer protocol field");
  if (v->data.integer != 1)
    return req_error(r, "unsupported protocol %lld (expected 1)",
                     (long long)v->data.integer);
  if (!r->invocation_id)
    return req_error(r, "missing invocation_id");

  r->tool = obj_field_str(obj, "tool");
  if (!r->tool) return req_error(r, "missing tool field");
  r->version = obj_field_str(obj, "version");
  if (!r->version) return req_error(r, "missing version field");
  r->command = obj_field_str(obj, "command");
  if (!r->command) return req_error(r, "missing command field");

  v = obj_field(obj, "args");
  if (!v || v->type != XCDN_VAL_OBJECT)
    return req_error(r, "missing args object");
  r->args = v;

  v = obj_field(obj, "grants");
  if (v && v->type == XCDN_VAL_OBJECT) r->grants = v;
  r->workspace = obj_field_str(obj, "workspace");
  r->scratch = obj_field_str(obj, "scratch");

  v = obj_field(obj, "limits");
  if (v && v->type == XCDN_VAL_OBJECT) {
    const xcdn_value_t *m = obj_field(v, "max_output_bytes");
    if (m && m->type == XCDN_VAL_INT && m->data.integer > 0)
      r->max_output_bytes = m->data.integer;
  }
  return 0;
}

void astd_req_free(astd_req *r) {
  if (!r) return;
  xcdn_document_free(r->doc);
  memset(r, 0, sizeof *r);
}

/* ---- argument accessors -------------------------------------------------- */

const xcdn_value_t *astd_arg(const astd_req *r, const char *name) {
  if (!r || !r->args || !name) return NULL;
  return obj_field(r->args, name);
}

const char *astd_arg_str(const astd_req *r, const char *name,
                         const char *dflt) {
  const xcdn_value_t *v = astd_arg(r, name);
  if (v && v->type == XCDN_VAL_STRING && v->data.string)
    return v->data.string;
  return dflt;
}

int64_t astd_arg_int(const astd_req *r, const char *name, int64_t dflt) {
  const xcdn_value_t *v = astd_arg(r, name);
  if (v && v->type == XCDN_VAL_INT) return v->data.integer;
  return dflt;
}

int astd_arg_bool(const astd_req *r, const char *name, int dflt) {
  const xcdn_value_t *v = astd_arg(r, name);
  if (v && v->type == XCDN_VAL_BOOL) return v->data.boolean ? 1 : 0;
  return dflt;
}

double astd_arg_num(const astd_req *r, const char *name, double dflt) {
  const xcdn_value_t *v = astd_arg(r, name);
  if (v && v->type == XCDN_VAL_FLOAT) return v->data.floating;
  if (v && v->type == XCDN_VAL_INT) return (double)v->data.integer;
  return dflt;
}

/* ---- glob ---------------------------------------------------------------- */

/* Parse a '[...]' class at *pp (pointing at '['). On a well-formed class,
 * advance *pp past the closing ']' and return 1 iff ch matches (negation
 * with a leading '!' or '^', ranges, ']' literal when first). On a
 * malformed class (no ']'), '[' is a literal. A class never matches '/'. */
static int glob_bracket(const char **pp, char ch) {
  const char *p = *pp + 1;
  const char *scan;
  int neg = 0, matched = 0;
  unsigned char c = (unsigned char)ch;
  if (*p == '!' || *p == '^') {
    neg = 1;
    p++;
  }
  scan = p;
  if (*scan == ']') scan++;
  while (*scan != '\0' && *scan != ']') scan++;
  if (*scan != ']') {
    *pp += 1;
    return ch == '[';
  }
  while (p < scan) {
    unsigned char lo = (unsigned char)*p;
    if (p + 2 < scan && p[1] == '-') {
      unsigned char hi = (unsigned char)p[2];
      if (c >= lo && c <= hi) matched = 1;
      p += 3;
    } else {
      if (c == lo) matched = 1;
      p += 1;
    }
  }
  if (neg) matched = !matched;
  if (ch == '/') matched = 0;
  *pp = scan + 1;
  return matched;
}

int astd_glob_match(const char *pattern, const char *path) {
  const char *p, *s;
  const char *star_p = NULL, *star_s = NULL;
  if (!pattern || !path) return 0;
  if (strchr(pattern, '/') == NULL) {
    const char *slash = strrchr(path, '/');
    if (slash) path = slash + 1;
  }
  p = pattern;
  s = path;
  while (*s != '\0') {
    if (*p == '*') {
      star_p = p;
      star_s = s;
      p++;
    } else {
      const char *np = p;
      int ok;
      if (*p == '?') {
        np = p + 1;
        ok = (*s != '/');
      } else if (*p == '[') {
        ok = glob_bracket(&np, *s);
      } else {
        np = p + 1;
        ok = (*p != '\0' && *p == *s);
      }
      if (ok) {
        p = np;
        s++;
      } else if (star_p != NULL && *star_s != '/') {
        /* backtrack: let the last '*' consume one more (non-'/') char */
        star_s++;
        s = star_s;
        p = star_p + 1;
      } else {
        return 0;
      }
    }
  }
  while (*p == '*') p++;
  return *p == '\0';
}

/* ---- binary sniff -------------------------------------------------------- */

int astd_looks_binary(const char *buf, size_t n) {
  size_t lim = n < 4096 ? n : 4096;
  if (!buf) return 0;
  return memchr(buf, '\0', lim) != NULL;
}
