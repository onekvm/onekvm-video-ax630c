/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "../src/ax_edid_storage.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int fail_rename, fail_sync;
int __real_rename(const char *, const char *);
int __real_fsync(int);
int __wrap_rename(const char *old, const char *new) {
    if (fail_rename) { fail_rename = 0; errno = EIO; return -1; }
    return __real_rename(old, new);
}
int __wrap_fsync(int fd) {
    if (fail_sync && --fail_sync == 0) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
static void check(const char *path, const uint8_t *expected) {
    uint8_t actual[257]; int fd = open(path, O_RDONLY);
    assert(fd >= 0 && read(fd, actual, sizeof(actual)) == 256);
    assert(!memcmp(actual, expected, 256)); close(fd);
}
static void no_temps(const char *directory) {
    DIR *dir = opendir(directory); assert(dir);
    struct dirent *item;
    while ((item = readdir(dir))) assert(strncmp(item->d_name, ".active.", 8));
    closedir(dir);
}
int main(void) {
    char directory[] = "/tmp/onekvm-edid-storage.XXXXXX";
    assert(mkdtemp(directory));
    char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/active.bin", directory);
    uint8_t old[256], next[256]; memset(old, 0x31, 256); memset(next, 0x52, 256);
    struct ax_edid_storage state;
    assert(!ax_edid_storage_prepare(&state, directory, old));
    assert(access(path, F_OK) && !ax_edid_storage_publish(&state));
    assert(!ax_edid_storage_finish(&state, 1)); check(path, old); no_temps(directory);
    for (int failure = 0; failure < 3; failure++) {
        assert(!ax_edid_storage_prepare(&state, directory, next)); check(path, old);
        if (failure == 0) fail_rename = 1;
        if (failure == 1) fail_sync = 1;
        int rc = ax_edid_storage_publish(&state);
        assert(failure == 2 ? !rc : rc < 0);
        assert(!ax_edid_storage_finish(&state, 0)); check(path, old); no_temps(directory);
    }
    fail_sync = 1;
    assert(ax_edid_storage_prepare(&state, directory, next) < 0);
    check(path, old); no_temps(directory);
    assert(!ax_edid_storage_prepare(&state, directory, next));
    assert(!ax_edid_storage_publish(&state));
    assert(!ax_edid_storage_finish(&state, 1)); check(path, next); no_temps(directory);
    unlink(path);
    assert(!ax_edid_storage_prepare(&state, directory, next));
    fail_sync = 1;
    assert(ax_edid_storage_publish(&state) < 0);
    assert(!ax_edid_storage_finish(&state, 0) && access(path, F_OK)); no_temps(directory);
    int fd = open(path, O_CREAT | O_WRONLY, 0600); assert(fd >= 0);
    assert(write(fd, old, 128) == 128); close(fd);
    assert(ax_edid_storage_prepare(&state, directory, next) < 0);
    struct stat st; assert(!stat(path, &st) && st.st_size == 128); no_temps(directory);
    unlink(path); assert(!symlink("/nonexistent", path));
    assert(ax_edid_storage_prepare(&state, directory, next) < 0);
    assert(!lstat(path, &st) && S_ISLNK(st.st_mode)); no_temps(directory);
    unlink(path); assert(!rmdir(directory));
    puts("EDID preference: staged commit, rename/fsync failure rollback and invalid-file preservation PASS");
    return 0;
}
