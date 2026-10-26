/* Independent blocking peer for hostile LSP and lifecycle contract tests. */
#define _POSIX_C_SOURCE 200809L
#include "asmodel_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static asmodel_json_value *read_message(void) {
  char line[512];
  size_t size = 0;
  while (fgets(line, sizeof line, stdin)) {
    if (!strcmp(line, "\r\n")) break;
    if (sscanf(line, "Content-Length: %zu", &size) != 1) return NULL;
  }
  if (!size || size > 2097152) return NULL;
  char *text = malloc(size);
  asmodel_json_value *v = NULL;
  if (text && fread(text, 1, size, stdin) == size) (void)asmodel_json_parse(text, size, &v);
  free(text);
  return v;
}
static void send_message(asmodel_json_value *v) {
  char *text = asmodel_json_write(v, 0);
  asmodel_json_free(v);
  if (!text) exit(2);
  printf("Content-Length: %zu\r\n\r\n%s", strlen(text), text);
  fflush(stdout);
  free(text);
}
static asmodel_json_value *json(const char *text) {
  asmodel_json_value *v = NULL;
  (void)asmodel_json_parse(text, strlen(text), &v);
  return v;
}
static void reply(long long id, asmodel_json_value *result) {
  asmodel_json_value *v = asmodel_json_object();
  asmodel_json_object_set(v, "jsonrpc", asmodel_json_string("2.0"));
  asmodel_json_object_set(v, "id", asmodel_json_int(id));
  asmodel_json_object_set(v, "result", result);
  send_message(v);
}
static void diagnostics(const char *uri, long long version, int current) {
  asmodel_json_value *v =
      json("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{}}");
  asmodel_json_value *p = asmodel_json_object_get(v, "params");
  asmodel_json_object_set(p, "uri", asmodel_json_string(uri));
  asmodel_json_object_set(p, "version", asmodel_json_int(version));
  asmodel_json_object_set(p, "diagnostics",
                current ? json("[{\"message\":\"current "
                               "error\",\"severity\":1,\"range\":{\"start\":{\"line\":0,"
                               "\"character\":0},\"end\":{\"line\":0,\"character\":1}}}]")
                        : asmodel_json_array());
  send_message(v);
}
int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "normal";
  char *uri = NULL;
  for (;;) {
    asmodel_json_value *message = read_message();
    if (!message) break;
    const char *method = asmodel_json_string_value(asmodel_json_object_get(message, "method"));
    long long id = asmodel_json_int_value(asmodel_json_object_get(message, "id"));
    asmodel_json_value *params = asmodel_json_object_get(message, "params");
    if (method && !strcmp(method, "initialize")) {
      asmodel_json_value *result = json(
          "{\"capabilities\":{\"positionEncoding\":\"utf-8\",\"textDocumentSync\":{\"openClose\":"
          "true},\"documentSymbolProvider\":true,\"definitionProvider\":true,"
          "\"referencesProvider\":true},\"serverInfo\":{\"name\":\"fixture\",\"version\":\"1\"}}");
      if (!strcmp(mode, "utf16"))
        asmodel_json_object_set(asmodel_json_object_get(result, "capabilities"), "positionEncoding",
                      asmodel_json_string("utf-16"));
      reply(id, result);
    } else if (method && !strcmp(method, "textDocument/didOpen")) {
      asmodel_json_value *doc = asmodel_json_object_get(params, "textDocument");
      free(uri);
      uri = strdup(asmodel_json_string_value(asmodel_json_object_get(doc, "uri")));
      if (!strcmp(mode, "diagnostics") || !strcmp(mode, "stale")) {
        long long version = asmodel_json_int_value(asmodel_json_object_get(doc, "version"));
        diagnostics(uri, version - 1, 0);
        if (!strcmp(mode, "stale")) diagnostics(uri, version - 2, 0);
        else diagnostics(uri, version, 1);
      }
    } else if (method && (!strcmp(method, "textDocument/documentSymbol") ||
                          !strcmp(method, "textDocument/definition"))) {
      if (!strcmp(mode, "hang")) {
        /* stderr is drained; the request marker is observed independently. */
        int first = 1;
        char *path = uri ? strdup(uri + 7) : NULL;
        if (path) {
          char *slash = strrchr(path, '/');
          if (slash) strcpy(slash + 1, "accepted");
          first = access(path, F_OK) != 0;
          FILE *f = fopen(path, "w");
          if (f) fclose(f);
          free(path);
        }
        while (first) {
          fputs("waiting\n", stderr);
          fflush(stderr);
          struct timespec ts = {0, 10000000};
          nanosleep(&ts, NULL);
        }
      }
      if (!strcmp(mode, "change")) {
        char *path = uri ? strdup(uri + 7) : NULL;
        if (path) {
          char *slash = strrchr(path, '/');
          if (slash) strcpy(slash + 1, "épreuve.cpp");
          FILE *f = fopen(path, "w");
          if (f) {
            fputs("int changed;\n", f);
            fclose(f);
          }
          free(path);
        }
      }
      if (!strcmp(mode, "error")) {
        asmodel_json_value *v = json("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32801}}");
        char detail[1600] = "";
        for (int i = 0; i < 200; i++) strcat(detail, "è😀");
        asmodel_json_object_set(v, "id", asmodel_json_int(id));
        asmodel_json_object_set(asmodel_json_object_get(v, "error"), "message", asmodel_json_string(detail));
        send_message(v);
      } else if (!strcmp(mode, "duplicate")) {
        const char *text = "{\"jsonrpc\":\"2.0\",\"id\":3,\"id\":3,\"result\":[]}";
        printf("Content-Length: %zu\r\n\r\n%s", strlen(text), text);
        fflush(stdout);
      } else if (!strcmp(mode, "request")) {
        send_message(json("{\"jsonrpc\":\"2.0\",\"id\":\"host-edit\",\"method\":\"workspace/"
                          "applyEdit\",\"params\":{}}"));
        asmodel_json_value *denial = read_message();
        int refused = asmodel_json_int_value(asmodel_json_object_get(asmodel_json_object_get(denial, "error"), "code")) == -32601;
        asmodel_json_free(denial);
        if (!refused) return 3;
        reply(id, asmodel_json_array());
      } else if (!strcmp(mode, "outside"))
        reply(id, json("[{\"uri\":\"file:///etc/"
                       "passwd\",\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{"
                       "\"line\":0,\"character\":1}}}]"));
      else reply(id + (!strcmp(mode, "wrongid") ? 1 : 0), asmodel_json_array());
    }
    asmodel_json_free(message);
  }
  free(uri);
  return 0;
}
