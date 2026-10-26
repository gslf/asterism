/* Live store host for the offline maintenance tests (scripts/store.py).
 *
 *   asper_store_host <root> [command...]
 *
 * Opens the store, runs each command in order, prints "ready", then holds
 * the store until stdin reaches EOF. Commands:
 *   insert-context <text>   semantic record in the context section
 *   source <scope> <text>   exact user event
 *   list-context            one line per context record
 *   stats                   "curation_deferred <n>"
 * Exit 0 on success; on failure the error name is printed to stderr. */
#include "asper.h"

#include <stdio.h>
#include <string.h>

static int fail(asper_ctx *c, const char *what, asper_err e) {
  fprintf(stderr, "%s: %s %s\n", what, asper_err_name(e),
          c ? asper_last_error(c) : "");
  asper_close(c);
  return 1;
}

int main(int argc, char **argv) {
  asper_open_params p = {0};
  asper_ctx *c = NULL;
  asper_err e;
  int i;
  if (argc < 2) {
    fputs("usage: asper_store_host <root> [command...]\n", stderr);
    return 2;
  }
  p.memory_root = argv[1];
  e = asper_open(&p, &c);
  if (e != ASPER_OK) return fail(NULL, "open", e);
  for (i = 2; i < argc; i++) {
    if (strcmp(argv[i], "insert-context") == 0 && i + 1 < argc) {
      char id[37];
      e = asper_memory_insert(c, ASPER_SECTION_CONTEXT, NULL, argv[++i], 0, id);
      if (e != ASPER_OK) return fail(c, "insert-context", e);
    } else if (strcmp(argv[i], "source") == 0 && i + 2 < argc) {
      asper_event_input event = {0};
      event.scope = argv[i + 1];
      event.kind = ASPER_EVENT_USER;
      event.text = argv[i + 2];
      i += 2;
      e = asper_event_append(c, &event, NULL);
      if (e != ASPER_OK) return fail(c, "source", e);
    } else if (strcmp(argv[i], "list-context") == 0) {
      asper_record **records = NULL;
      size_t n = 0, k;
      e = asper_memory_list(c, ASPER_SECTION_CONTEXT, NULL, 0, &records, &n);
      if (e != ASPER_OK) return fail(c, "list-context", e);
      for (k = 0; k < n; k++) printf("%s\n", asper_record_content(records[k]));
      asper_records_free(records, n);
    } else if (strcmp(argv[i], "stats") == 0) {
      asper_stats st;
      e = asper_get_stats(c, &st);
      if (e != ASPER_OK) return fail(c, "stats", e);
      printf("curation_deferred %zu\n", st.curation_deferred);
    } else {
      fprintf(stderr, "unknown command: %s\n", argv[i]);
      asper_close(c);
      return 2;
    }
  }
  puts("ready");
  fflush(stdout);
  while (getchar() != EOF) {
  }
  asper_close(c);
  return 0;
}
