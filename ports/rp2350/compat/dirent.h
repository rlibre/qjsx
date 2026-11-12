/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/* <dirent.h> for the RP2350: the newlib of the Arm toolchain refuses it (no
 * file system). common/runtime/files.cpp lists directories with it; the
 * RP2350 boards have no file system yet (board::dataDirectory() is nullptr),
 * so opendir() always fails with ENOSYS (posix_compat.cpp). The microSD card
 * (FatFS) will replace this. This folder comes before the system headers in
 * the include path. */
#ifndef QJSX_COMPAT_DIRENT_H
#define QJSX_COMPAT_DIRENT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct QjsxDirectory DIR;   /* never created */

struct dirent {
    char d_name[256];   /* name of the entry */
};

DIR           *opendir(const char *path);
struct dirent *readdir(DIR *directory);
int            closedir(DIR *directory);

#ifdef __cplusplus
}
#endif

#endif /* QJSX_COMPAT_DIRENT_H */
