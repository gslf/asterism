/*
 * os.h — the platform layer as seen by asper.
 *
 * The shared shim comes from core.h. Its fallible calls are wrapped here
 * so that asper code receives asper_err. The private-file helpers below are
 * asper-only (os_blob.c, os_stamp.c).
 */

#ifndef ASPER_OS_H
#define ASPER_OS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "asper.h"
#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

asper_err asper_from_core(core_err e);

#define os_thread_start(...) asper_from_core(os_thread_start(__VA_ARGS__))
#define os_file_replace(...) asper_from_core(os_file_replace(__VA_ARGS__))
#define os_fsync(...)        asper_from_core(os_fsync(__VA_ARGS__))
#define os_sync_parent(...)  asper_from_core(os_sync_parent(__VA_ARGS__))
#define os_mkdir_p(...)      asper_from_core(os_mkdir_p(__VA_ARGS__))
#define os_read_file(...)    asper_from_core(os_read_file(__VA_ARGS__))
#define os_write_file(...)   asper_from_core(os_write_file(__VA_ARGS__))
#define os_remove_file(...)  asper_from_core(os_remove_file(__VA_ARGS__))
#define os_remove_dir(...)   asper_from_core(os_remove_dir(__VA_ARGS__))
#define os_truncate(...)     asper_from_core(os_truncate(__VA_ARGS__))
#define os_file_size(...)    asper_from_core(os_file_size(__VA_ARGS__))
#define os_rename(...)       asper_from_core(os_rename(__VA_ARGS__))
#define os_list_dir(...)     asper_from_core(os_list_dir(__VA_ARGS__))
#define os_list_dirs(...)    asper_from_core(os_list_dirs(__VA_ARGS__))
#define os_realpath(...)     asper_from_core(os_realpath(__VA_ARGS__))

/* Create a new private regular file; never truncate an existing leaf/alias. */
FILE *os_blob_create(const char *path);
/* Open a regular read stream; reject aliases and special files. Caller closes it. */
asper_err os_blob_open(const char *path, FILE **out, uint64_t *size);
/* Resolve an operator-selected existing store directory once, before use. */
asper_err os_directory_canonical(const char *path, char **out);
typedef struct {
  uint64_t size, links;
  uint64_t modified[2], changed[2];
} os_file_stamp;
asper_err os_stream_stamp(FILE *stream, os_file_stamp *stamp);

#ifdef __cplusplus
}
#endif

#endif /* ASPER_OS_H */
