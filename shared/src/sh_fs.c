/*
 * sh_fs.c - Filesystem Utilities
 *
 * Portable directory creation without shell execution.
 */

#include "sh_fs.h"
#include <sys/stat.h>
#include <errno.h>
#include <string.h>

#include "sh_pal.h"

#define SH_FS_MAX_PATH 4096

int sh_mkdirs(const char *path)
{
    if (!path || !path[0]) return -1;

    size_t len = strlen(path);
    if (len >= SH_FS_MAX_PATH) {
        errno = ENAMETOOLONG;
        return -1;
    }

    char tmp[SH_FS_MAX_PATH];
    memcpy(tmp, path, len + 1);

    /* Remove trailing slash (unless root "/") */
    if (len > 1 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    /* Walk path creating each component */
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (sh_pal_mkdir(tmp) != 0) return -1;
            *p = '/';
        }
    }
    return sh_pal_mkdir(tmp);
}
