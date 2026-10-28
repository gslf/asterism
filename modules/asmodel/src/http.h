/* One bounded HTTP exchange for the ChatGPT plan provider and its OAuth calls.
 * libcurl where available, WinHTTP on Windows. Redirects are never followed:
 * a bearer credential must not leave the configured origin. */
#ifndef ASMODEL_HTTP_H
#define ASMODEL_HTTP_H
#include <stddef.h>
#include <stdint.h>

enum {
  ASMODEL_HTTP_OK = 0,          /* exchange completed; inspect status */
  ASMODEL_HTTP_TRANSPORT = -1,  /* connection, TLS or protocol failure */
  ASMODEL_HTTP_TIMEOUT = -2,    /* timeout_ms expired */
  ASMODEL_HTTP_CANCELLED = -3,  /* *cancel became nonzero */
  ASMODEL_HTTP_ABORTED = -4,    /* the sink asked to stop */
  ASMODEL_HTTP_UNAVAILABLE = -5 /* built without an HTTP client */
};

/* Called for every received body chunk; nonzero return aborts the exchange. */
typedef int (*asmodel_http_sink)(long status, const char *data, size_t len, void *ud);

typedef struct {
  const char *method;          /* "GET" or "POST" */
  const char *url;
  const char *const *headers;  /* "Name: value" lines, NULL-terminated */
  const char *body;            /* POST body, NUL-terminated */
  volatile int *cancel;
  int64_t timeout_ms;          /* whole exchange; 0 = unbounded */
  asmodel_http_sink sink;
  void *sink_ud;
  size_t max_body;             /* captured bytes; 0 = 16 MiB */
} asmodel_http_request;

typedef struct {
  long status;
  char *body;                  /* captured body, NUL-terminated, may be NULL */
  size_t len;
  char retry_after[64];        /* Retry-After header, verbatim */
  char request_id[128];        /* x-request-id / openai-request-id */
} asmodel_http_response;

int asmodel_http_available(void);
int asmodel_http_perform(const asmodel_http_request *rq, asmodel_http_response *rs,
                         char *error, size_t error_size);
void asmodel_http_response_free(asmodel_http_response *rs);
/* https://..., or plain http only to a loopback host (local tests/proxies). */
int asmodel_http_url_allowed(const char *url);
#endif
