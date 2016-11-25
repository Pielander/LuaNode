// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/errno.h>
#include "esp_vfs.h"
#include "esp_log.h"

/*
 * File descriptors visible by the applications are composed of two parts.
 * Lower CONFIG_MAX_FD_BITS bits are used for the actual file descriptor.
 * Next (16 - CONFIG_MAX_FD_BITS - 1) bits are used to identify the VFS this
 * descriptor corresponds to.
 * Highest bit is zero.
 * We can only use 16 bits because newlib stores file descriptor as short int.
 */

#ifndef CONFIG_MAX_FD_BITS
#define CONFIG_MAX_FD_BITS 12
#endif

#define MAX_VFS_ID_BITS (16 - CONFIG_MAX_FD_BITS - 1)
// mask of actual file descriptor (e.g. 0x00000fff)
#define VFS_FD_MASK     ((1 << CONFIG_MAX_FD_BITS) - 1)
// max number of VFS entries
#define VFS_MAX_COUNT   ((1 << MAX_VFS_ID_BITS) - 1)
// mask of VFS id (e.g. 0x00007000)
#define VFS_INDEX_MASK  (VFS_MAX_COUNT << CONFIG_MAX_FD_BITS)
#define VFS_INDEX_S     CONFIG_MAX_FD_BITS

typedef struct vfs_entry_ {
    esp_vfs_t vfs;          // contains pointers to VFS functions
    char path_prefix[ESP_VFS_PATH_MAX]; // path prefix mapped to this VFS
    size_t path_prefix_len; // micro-optimization to avoid doing extra strlen
    void* ctx;              // optional pointer which can be passed to VFS
    int offset;             // index of this structure in s_vfs array
} vfs_entry_t;

static vfs_entry_t* s_vfs[VFS_MAX_COUNT] = { 0 };
static size_t s_vfs_count = 0;

esp_err_t esp_vfs_register(const char* base_path, const esp_vfs_t* vfs, void* ctx)
{
    if (s_vfs_count >= VFS_MAX_COUNT) {
        return ESP_ERR_NO_MEM;
    }
    size_t len = strlen(base_path);
    if (len < 2 || len > ESP_VFS_PATH_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (base_path[0] != '/' || base_path[len - 1] == '/') {
        return ESP_ERR_INVALID_ARG;
    }
    vfs_entry_t *entry = (vfs_entry_t*) malloc(sizeof(vfs_entry_t));
    if (entry == NULL) {
        return ESP_ERR_NO_MEM;
    }
    strcpy(entry->path_prefix, base_path); // we have already verified argument length
    memcpy(&entry->vfs, vfs, sizeof(esp_vfs_t));
    entry->path_prefix_len = len;
    entry->ctx = ctx;
    entry->offset = s_vfs_count;
    s_vfs[s_vfs_count] = entry;
    ++s_vfs_count;
    return ESP_OK;
}

static const vfs_entry_t* get_vfs_for_fd(int fd)
{
    int index = ((fd & VFS_INDEX_MASK) >> VFS_INDEX_S);
    if (index >= s_vfs_count) {
        return NULL;
    }
    return s_vfs[index];
}

static int translate_fd(const vfs_entry_t* vfs, int fd)
{
    return (fd & VFS_FD_MASK) + vfs->vfs.fd_offset;
}

static const char* translate_path(const vfs_entry_t* vfs, const char* src_path)
{
    assert(strncmp(src_path, vfs->path_prefix, vfs->path_prefix_len) == 0);
    return src_path + vfs->path_prefix_len;
}

static const vfs_entry_t* get_vfs_for_path(const char* path)
{
    size_t len = strlen(path);
    for (size_t i = 0; i < s_vfs_count; ++i) {
        const vfs_entry_t* vfs = s_vfs[i];
        if (len < vfs->path_prefix_len + 1) {   // +1 is for the trailing slash after base path
            continue;
        }
        if (memcmp(path, vfs->path_prefix, vfs->path_prefix_len) != 0) {  // match prefix
            continue;
        }
        if (path[vfs->path_prefix_len] != '/') {   // don't match "/data" prefix for "/data1/foo.txt"
            continue;
        }
        return vfs;
    }
    return NULL;
}

/*
 * Using huge multi-line macros is never nice, but in this case
 * the only alternative is to repeat this chunk of code (with different function names)
 * for each syscall being implemented. Given that this define is contained within a single
 * file, this looks like a good tradeoff.
 *
 * First we check if syscall is implemented by VFS (corresponding member is not NULL),
 * then call the right flavor of the method (e.g. open or open_p) depending on
 * ESP_VFS_FLAG_CONTEXT_PTR flag. If ESP_VFS_FLAG_CONTEXT_PTR is set, context is passed
 * in as first argument and _p variant is used for the call.
 * It is enough to check just one of them for NULL, as both variants are part of a union.
 */
#define CHECK_AND_CALL(ret, r, pvfs, func, ...) \
    if (pvfs->vfs.func == NULL) { \
        __errno_r(r) = ENOSYS; \
        return -1; \
    } \
    if (pvfs->vfs.flags & ESP_VFS_FLAG_CONTEXT_PTR) { \
        ret = (*pvfs->vfs.func ## _p)(pvfs->ctx, __VA_ARGS__); \
    } else { \
        ret = (*pvfs->vfs.func)(__VA_ARGS__);\
    }


int esp_vfs_open(struct _reent *r, const char * path, int flags, int mode)
{
    const vfs_entry_t* vfs = get_vfs_for_path(path);
    if (vfs == NULL) {
        __errno_r(r) = ENOENT;
        return -1;
    }
    const char* path_within_vfs = translate_path(vfs, path);
    int ret;
    CHECK_AND_CALL(ret, r, vfs, open, path_within_vfs, flags, mode);
    if (ret < 0) {
        return ret;
    }
    assert(ret >= vfs->vfs.fd_offset);
    return ret - vfs->vfs.fd_offset + (vfs->offset << VFS_INDEX_S);
}

ssize_t esp_vfs_write(struct _reent *r, int fd, const void * data, size_t size)
{
    const vfs_entry_t* vfs = get_vfs_for_fd(fd);
    if (vfs == NULL) {
        __errno_r(r) = EBADF;
        return -1;
    }
    int local_fd = translate_fd(vfs, fd);
    int ret;
    CHECK_AND_CALL(ret, r, vfs, write, local_fd, data, size);
    return ret;
}

off_t esp_vfs_lseek(struct _reent *r, int fd, off_t size, int mode)
{
    const vfs_entry_t* vfs = get_vfs_for_fd(fd);
    if (vfs == NULL) {
        __errno_r(r) = EBADF;
        return -1;
    }
    int local_fd = translate_fd(vfs, fd);
    int ret;
    CHECK_AND_CALL(ret, r, vfs, lseek, local_fd, size, mode);
    return ret;
}

ssize_t esp_vfs_read(struct _reent *r, int fd, void * dst, size_t size)
{
    const vfs_entry_t* vfs = get_vfs_for_fd(fd);
    if (vfs == NULL) {
        __errno_r(r) = EBADF;
        return -1;
    }
    int local_fd = translate_fd(vfs, fd);
    int ret;
    CHECK_AND_CALL(ret, r, vfs, read, local_fd, dst, size);
    return ret;
}


int esp_vfs_close(struct _reent *r, int fd)
{
    const vfs_entry_t* vfs = get_vfs_for_fd(fd);
    if (vfs == NULL) {
        __errno_r(r) = EBADF;
        return -1;
    }
    int local_fd = translate_fd(vfs, fd);
    int ret;
    CHECK_AND_CALL(ret, r, vfs, close, local_fd);
    return ret;
}

int esp_vfs_fstat(struct _reent *r, int fd, struct stat * st)
{
    const vfs_entry_t* vfs = get_vfs_for_fd(fd);
    if (vfs == NULL) {
        __errno_r(r) = EBADF;
        return -1;
    }
    int local_fd = translate_fd(vfs, fd);
    int ret;
    CHECK_AND_CALL(ret, r, vfs, fstat, local_fd, st);
    return ret;
}

int esp_vfs_stat(struct _reent *r, const char * path, struct stat * st)
{
    const vfs_entry_t* vfs = get_vfs_for_path(path);
    if (vfs == NULL) {
        __errno_r(r) = ENOENT;
        return -1;
    }
    const char* path_within_vfs = translate_path(vfs, path);
    int ret;
    CHECK_AND_CALL(ret, r, vfs, stat, path_within_vfs, st);
    return ret;
}

int esp_vfs_link(struct _reent *r, const char* n1, const char* n2)
{
    const vfs_entry_t* vfs = get_vfs_for_path(n1);
    if (vfs == NULL) {
        __errno_r(r) = ENOENT;
        return -1;
    }
    const vfs_entry_t* vfs2 = get_vfs_for_path(n2);
    if (vfs != vfs2) {
        __errno_r(r) = EXDEV;
        return -1;
    }
    const char* path1_within_vfs = translate_path(vfs, n1);
    const char* path2_within_vfs = translate_path(vfs, n2);
    int ret;
    CHECK_AND_CALL(ret, r, vfs, link, path1_within_vfs, path2_within_vfs);
    return ret;
}

int esp_vfs_unlink(struct _reent *r, const char *path)
{
    const vfs_entry_t* vfs = get_vfs_for_path(path);
    if (vfs == NULL) {
        __errno_r(r) = ENOENT;
        return -1;
    }
    const char* path_within_vfs = translate_path(vfs, path);
    int ret;
    CHECK_AND_CALL(ret, r, vfs, unlink, path_within_vfs);
    return ret;
}

int esp_vfs_rename(struct _reent *r, const char *src, const char *dst)
{
    const vfs_entry_t* vfs = get_vfs_for_path(src);
    if (vfs == NULL) {
        __errno_r(r) = ENOENT;
        return -1;
    }
    const vfs_entry_t* vfs_dst = get_vfs_for_path(dst);
    if (vfs != vfs_dst) {
        __errno_r(r) = EXDEV;
        return -1;
    }
    const char* src_within_vfs = translate_path(vfs, src);
    const char* dst_within_vfs = translate_path(vfs, dst);
    int ret;
    CHECK_AND_CALL(ret, r, vfs, rename, src_within_vfs, dst_within_vfs);
    return ret;
}