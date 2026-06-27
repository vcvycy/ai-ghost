/* =============================================================
 *  simple_socket.cpp —— SimpleSocket 类实现
 * ============================================================= */
#include "simple_socket.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

/* ---------- ctor / dtor / 移动语义 ---------- */

SimpleSocket::SimpleSocket(int family, int type, int proto)
    : fd_(-1) {
    fd_ = ::socket(family, type, proto);
    if (fd_ < 0) {
        throw std::runtime_error(
            std::string("socket(): ") + strerror(errno));
    }
}

SimpleSocket::~SimpleSocket() {
    close();
}

SimpleSocket::SimpleSocket(SimpleSocket&& other) noexcept
    : fd_(other.fd_) {
    other.fd_ = -1;
}

SimpleSocket& SimpleSocket::operator=(SimpleSocket&& other) noexcept {
    if (this != &other) {
        close();              /* 把自己先关掉 */
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

/* ---------- settimeout(seconds) ----------
 * seconds <= 0 视作 "关闭超时"(python 传 None 的语义)
 * 这里实现里直接传一个 {0,0} 的 timeval 给 setsockopt,
 * 内核会把它当作 "无限等待",效果等同阻塞。
 */
void SimpleSocket::settimeout(double seconds) {
    if (fd_ < 0) {
        throw std::runtime_error("settimeout on closed socket");
    }
    struct timeval tv;
    if (seconds <= 0.0) {
        tv.tv_sec  = 0;
        tv.tv_usec = 0;
    } else {
        tv.tv_sec  = static_cast<time_t>(seconds);
        tv.tv_usec = static_cast<suseconds_t>(
            (seconds - static_cast<double>(tv.tv_sec)) * 1e6);
    }
    if (::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0 ||
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        throw std::runtime_error(
            std::string("setsockopt(SO_*_TIMEO): ") + strerror(errno));
    }
}

/* ---------- connect(host, port) ---------- */
void SimpleSocket::connect(const std::string& host, int port) {
    if (fd_ < 0) {
        throw std::runtime_error("connect on closed socket");
    }
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("inet_pton: invalid address: " + host);
    }
    if (::connect(fd_, reinterpret_cast<struct sockaddr *>(&addr),
                  sizeof(addr)) < 0) {
        throw std::runtime_error(
            std::string("connect(): ") + strerror(errno));
    }
}

/* ---------- sendall ---------- */
void SimpleSocket::sendall(const void *buf, std::size_t len) {
    if (fd_ < 0) {
        throw std::runtime_error("sendall on closed socket");
    }
    const char *p = static_cast<const char *>(buf);
    while (len > 0) {
        ssize_t n = ::send(fd_, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;   /* 信号打断,继续 */
            throw std::runtime_error(
                std::string("send(): ") + strerror(errno));
        }
        if (n == 0) {
            /* 理论上 send 返回 0 不太会发生,但保险起见当对端断 */
            throw std::runtime_error("send(): peer closed");
        }
        p   += n;
        len -= static_cast<std::size_t>(n);
    }
}

void SimpleSocket::sendall(const std::string &data) {
    sendall(data.data(), data.size());
}

/* ---------- recv ----------
 * python 语义:
 *   - 返回 bytes:正常数据
 *   - 返回 b"":对端正常关闭
 *   - 抛 socket.timeout:超时
 *   - 抛其它 OSError:其它错误
 * 这里统一:失败抛 std::runtime_error,正常关闭返回空串。
 */
std::string SimpleSocket::recv(std::size_t maxsize) {
    if (fd_ < 0) {
        throw std::runtime_error("recv on closed socket");
    }
    std::string out;
    out.resize(maxsize);

    ssize_t n;
    do {
        n = ::recv(fd_, &out[0], maxsize, 0);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        throw std::runtime_error(
            std::string("recv(): ") + strerror(errno));
    }
    if (n == 0) return std::string();    /* 对端关闭 */
    out.resize(static_cast<std::size_t>(n));
    return out;
}

/* ---------- close ---------- */
void SimpleSocket::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}