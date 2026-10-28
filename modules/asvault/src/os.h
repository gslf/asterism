/*
 * os.h — the platform layer as seen by asvault.
 *
 * The shared shim comes from core.h. Its fallible calls are wrapped here so
 * that asvault code receives asvault_err. The private-file and terminal
 * helpers below are asvault-only (os_private.c): owner-only files created
 * exclusively, unbuffered reads of key material, and no-echo terminal input.
 */

#ifndef ASVAULT_OS_H
#define ASVAULT_OS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "asvault.h"
#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

asvault_err asvault_from_core(core_err e);

#define os_mkdir_p(...)     asvault_from_core(os_mkdir_p(__VA_ARGS__))
#define os_read_file(...)   asvault_from_core(os_read_file(__VA_ARGS__))
#define os_file_replace(...) asvault_from_core(os_file_replace(__VA_ARGS__))
#define os_remove_file(...) asvault_from_core(os_remove_file(__VA_ARGS__))
#define os_sync_parent(...) asvault_from_core(os_sync_parent(__VA_ARGS__))

/* Create path and parents; the leaf is made owner-only (0700 on POSIX). */
asvault_err os_private_dir(const char *path);
/* Create a new owner-only file (0600 on POSIX) holding data, synced. Never
 * replaces anything: an existing path (or alias) fails with
 * ASVAULT_ERR_IO. */
asvault_err os_private_create(const char *path, const void *data, size_t len);
/* Atomically replace path with an owner-only file holding data, through
 * "<path>.tmp". The caller holds the writer lock. */
asvault_err os_private_replace(const char *path, const void *data, size_t len);
/* Read a whole small regular file without stdio buffering, so key material
 * is copied only into buf. ASVAULT_ERR_NOT_FOUND when missing,
 * ASVAULT_ERR_INVALID when longer than cap or not a regular file. */
asvault_err os_private_read(const char *path, void *buf, size_t cap,
                            size_t *out_len);
/* Permission bits of path (POSIX); 0 when the platform has none to report
 * or the path is missing. */
int os_private_mode(const char *path, unsigned *out_mode);

/* 1 when standard input is a terminal. */
int os_stdin_is_tty(void);
/* Read standard input without stdio buffering into buf (cap bytes,
 * NUL-terminated). With prompt NULL: to end of input. With a prompt: one
 * line from the terminal with echo off; the prompt goes to stderr once echo
 * is off, and echo is restored after, also when a signal interrupts the
 * read. *out_len is the byte count; more than cap - 1 bytes fails with
 * ASVAULT_ERR_INVALID. */
asvault_err os_read_stdin(char *buf, size_t cap, size_t *out_len,
                          const char *prompt);

#ifdef __cplusplus
}
#endif

#endif /* ASVAULT_OS_H */
