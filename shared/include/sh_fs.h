/*
 * sh_fs.h - Filesystem Utilities
 *
 * Safe, portable filesystem operations. No shell execution (no system()).
 */

#ifndef SH_FS_H
#define SH_FS_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Recursively create directories (equivalent to "mkdir -p").
 *
 * Creates all intermediate directories as needed. Thread-safe.
 * Uses mode 0755 for created directories.
 *
 * @param path  Directory path to create
 * @return 0 on success, -1 on error (errno set)
 */
int sh_mkdirs(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* SH_FS_H */
