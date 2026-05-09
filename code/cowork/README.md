# TermSync 终端多人协作代码编辑器（C 版）

这是一个面向 Linux/WSL 的 C 语言终端协作编辑器。项目采用“服务端主导 OT（Operational Transformation）”方案：客户端只负责收集输入并按 ACK 节奏发送，服务端作为唯一权威状态源，统一排序、变换、应用和广播编辑操作。

## 构建

依赖：

- `gcc`
- POSIX socket / `pthread` / `fork` / pipe
- `ncursesw`

```bash
make
```

构建产物：

```text
build/termsync_server
build/termsync_client
build/test_termsync
```

## 运行

启动服务端：

```bash
./build/termsync_server --host 0.0.0.0 --port 8765 --file shared.c
```

启动两个客户端：

```bash
./build/termsync_client --host glance02.xyz --port 8765 --client-id u1
./build/termsync_client --host glance02.xyz --port 8765 --client-id u2
```

客户端快捷键：

- `Ctrl-S`：请求服务端手动保存。
- `Ctrl-Q`：退出客户端。
- 方向键：移动光标。
- 普通字符：插入字符，支持 UTF-8 中文字符。
- Backspace：删除光标前一个字符。

## 文件

```text
Makefile              # 构建 server/client/test
termsync.h            # 公共类型和函数声明
protocol.c            # JSON Lines 编码、解析和 socket 收发
document.c            # 权威文档、OT、版本历史和重同步
client_state.c        # 客户端 ACK 队列和远程操作处理
autosave.c            # fork 保存进程、pipe 事件、备份和日志
server.c              # TCP 多线程服务端
client.c              # ncursesw 终端客户端
tests/test_termsync.c # 自带轻量测试 runner
```

服务端维护的运行期文件以 `--file shared.c` 为例：

```text
shared.c
shared.c.bak
shared.c.autosave
shared.c.edit.log
```

## 测试

```bash
make test
```

测试覆盖服务端版本递增、OT 位置变换、重复删除转 `noop`、历史过期重同步、UTF-8 中文字符处理、客户端 ACK 队列、远程版本缺口、JSON Lines 编解码、手动保存备份和自动保存文件写入。
