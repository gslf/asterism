/* Internal seams of the ChatGPT plan provider: the Responses event decoder,
 * the credential store used by inference, and the provider constructor. */
#ifndef ASMODEL_CHATGPT_H
#define ASMODEL_CHATGPT_H
#include "asmodel.h"

enum {
  CHATGPT_STREAM_NONE = 0,
  CHATGPT_STREAM_COMPLETED,  /* response.completed: the only success signal */
  CHATGPT_STREAM_INCOMPLETE, /* response.incomplete */
  CHATGPT_STREAM_FAILED,     /* response.failed */
  CHATGPT_STREAM_ERROR       /* error event */
};

/* Incremental Server-Sent Events decoder for the Responses stream. Protocol
 * fields are read by structure; content cannot impersonate usage or status. */
typedef struct {
  char *line; size_t line_n, line_cap;
  char *data; size_t data_n, data_cap;
  int has_data;
  char *text; size_t text_n, text_cap;
  size_t text_limit;          /* bytes; 0 = unbounded */
  asmodel_tool_calls *calls;  /* NULL: a function call is a violation */
  int calls_seen, text_seen;
  int terminal, limit_hit, bad, refused;
  int input_tokens, output_tokens, reasoning_tokens, cached_tokens;
  int usage_known, reasoning_known;
  char code[64], message[256], param[96], reason[64];
} chatgpt_stream;

void chatgpt_stream_init(chatgpt_stream *s, asmodel_tool_calls *calls, size_t text_limit);
/* Nonzero when reading should stop: output limit, violation or terminal failure. */
int  chatgpt_stream_feed(chatgpt_stream *s, const char *p, size_t n);
void chatgpt_stream_end(chatgpt_stream *s); /* dispatch a final unterminated event */
char *chatgpt_stream_take_text(chatgpt_stream *s); /* malloc-owned, NULL if none */
void chatgpt_stream_free(chatgpt_stream *s);

/* Bearer token for inference, refreshed under the process and file locks when
 * near expiry, or when `stale` (a token the server just rejected) is still the
 * stored one. AUTH: not logged in or login expired. UNSUPPORTED: plan usage
 * not granted. BACKEND: transient failure, credentials kept. */
asmodel_err asmodel_chatgpt_access(const char *auth_file, const char *stale, int64_t timeout_ms,
                                   volatile int *cancel, char **out_token,
                                   char *error, size_t error_size);

int asmodel_chatgpt_provider_create(const asmodel_spec *spec, asmodel_provider *out,
                                    char *error, size_t error_size);

/* Printable, bounded copy of untrusted diagnostic text. */
void asmodel_chatgpt_sanitize(char *dst, size_t cap, const char *src);
#endif
