/*
 * os.h — the platform layer as seen by asngn.
 *
 * The shim comes from core.h. Its fallible calls are wrapped here so that
 * engine code receives asngn_err.
 */

#ifndef ASNGN_OS_H
#define ASNGN_OS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "asngn.h"
#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

asngn_err asngn_from_core(core_err e);

/* Canonical path of an existing entry, malloc'd; NULL on any failure. */
char *asngn_realpath(const char *path);

#define os_thread_start(...) asngn_from_core(os_thread_start(__VA_ARGS__))
#define os_file_replace(...) asngn_from_core(os_file_replace(__VA_ARGS__))
#define os_fsync(...)        asngn_from_core(os_fsync(__VA_ARGS__))
#define os_sync_parent(...)  asngn_from_core(os_sync_parent(__VA_ARGS__))
#define os_mkdir_p(...)      asngn_from_core(os_mkdir_p(__VA_ARGS__))
#define os_read_file(...)    asngn_from_core(os_read_file(__VA_ARGS__))
#define os_write_file(...)   asngn_from_core(os_write_file(__VA_ARGS__))
#define os_remove_file(...)  asngn_from_core(os_remove_file(__VA_ARGS__))
#define os_remove_dir(...)   asngn_from_core(os_remove_dir(__VA_ARGS__))
#define os_truncate(...)     asngn_from_core(os_truncate(__VA_ARGS__))
#define os_file_size(...)    asngn_from_core(os_file_size(__VA_ARGS__))
#define os_rename(...)       asngn_from_core(os_rename(__VA_ARGS__))
#define os_list_dir(...)     asngn_from_core(os_list_dir(__VA_ARGS__))
#define os_list_dirs(...)    asngn_from_core(os_list_dirs(__VA_ARGS__))

#ifdef __cplusplus
}
#endif

#endif /* ASNGN_OS_H */
