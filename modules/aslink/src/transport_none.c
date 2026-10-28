/*
 * transport_none.c — aslink without an HTTP backend (neither libcurl nor
 * WinHTTP): every exchange reports ASLINK_ERR_UNSUPPORTED.
 */

#include "aslink_internal.h"

aslink_err aslink_perform(const aslink_xfer *x, char **out_body, size_t *out_len,
                          long *out_status, aslink_status *st) {
  (void)x;
  if (out_body) *out_body = NULL;
  if (out_len) *out_len = 0;
  if (out_status) *out_status = 0;
  aslink_status_msg(st, NULL, "aslink was built without libcurl or WinHTTP");
  return ASLINK_ERR_UNSUPPORTED;
}

int aslink_has_transport(void) { return 0; }
