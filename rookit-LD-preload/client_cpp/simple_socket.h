/* =============================================================
 *  simple_socket.h
 *  一个轻量级 C++ socket 封装,接口尽量贴近 Python socket 库,
 *  方便从 client.py 风格的代码平滑迁移到 C++。
 *
 *  用法示例:
 *      SimpleSocket sock;
 *      sock.settimeout(30);                       // 秒
 *      sock.connect("127.0.0.1", 4444);
 *      sock.sendall("[+] hello\n");
 *      std::string data = sock.recv(4096);        // "" 表示对端关闭
 *
 *  设计要点:
 *    - RAII:构造拿 fd,析构自动 close
 *    - 禁用拷贝,允许移动(所有权转移,避免 double-close)
 *    - 失败抛 std::runtime_error(类似 python 的 socket.error)
 *    - 暴露 fileno() 给 select/poll/daemonize 等底层场景
 *    - 默认参数直接复用 sys/socket.h 里的 AF_INET / SOCK_STREAM
 * ============================================================= */
#ifndef SIMPLE_SOCKET_H
#define SIMPLE_SOCKET_H

#include <sys/socket.h>   /* AF_INET / SOCK_STREAM,默认参数要用 */
#include <cstddef>
#include <string>

class SimpleSocket {
public:
    /* python: socket.socket(family=AF_INET, type=SOCK_STREAM, proto=0)
     * 失败抛 std::runtime_error */
    SimpleSocket(int family = AF_INET,
                 int type   = SOCK_STREAM,
                 int proto  = 0);

    /* 析构自动 close */
    ~SimpleSocket();

    /* 禁止拷贝 —— 否则会 double-close;允许移动 —— 转移所有权 */
    SimpleSocket(const SimpleSocket&)            = delete;
    SimpleSocket& operator=(const SimpleSocket&) = delete;
    SimpleSocket(SimpleSocket&& other) noexcept;
    SimpleSocket& operator=(SimpleSocket&& other) noexcept;

    /* python: sock.settimeout(seconds)
     * seconds <= 0 表示阻塞(不设超时)
     * 同时设置收发超时,与 python 行为一致 */
    void settimeout(double seconds);

    /* python: sock.connect((host, port))
     * 只支持 IPv4(host 必须是点分十进制字符串) */
    void connect(const std::string& host, int port);

    /* python: sock.sendall(data)
     * 重载 std::string 和 (buf, len) 两种形式;循环 send 直到发完 */
    void sendall(const std::string& data);
    void sendall(const void* buf, std::size_t len);

    /* python: sock.recv(bufsize)
     * 返回 "" 表示对端关闭;其它错误抛 std::runtime_error */
    std::string recv(std::size_t maxsize);

    /* python: sock.close() —— 重复调用安全 */
    void close();

    /* python: sock.fileno() —— 给 select/poll 用 */
    int fileno() const { return fd_; }

    /* python: bool(sock) —— fd 还活着就是 true */
    explicit operator bool() const { return fd_ >= 0; }

private:
    int fd_;
};

#endif /* SIMPLE_SOCKET_H */