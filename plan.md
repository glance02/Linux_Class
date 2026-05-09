# TermSync C 版实现计划

实现一个面向 Linux/WSL 的 C 语言终端多人协作代码编辑器。服务端是唯一权威状态源，所有编辑操作进入文档处理临界区后才确定全局顺序；客户端只维护输入队列，保证连续按键不丢失，并按 ACK 节奏逐个发送。

## 目标

- 使用 POSIX socket 实现 TCP 服务端和客户端。
- 使用 `pthread` 为每个客户端连接创建独立处理线程。
- 使用服务端主导 OT 处理旧版本编辑操作。
- 使用 `fork()` 和 pipe 将自动保存、手动保存、备份和日志写入放到独立进程。
- 使用 `ncursesw` 实现终端界面和 UTF-8 输入。
- 使用自带 C 测试 runner 验证核心同步、协议和保存行为。

## 模块

- `termsync.h`：公共类型、消息结构和函数声明。
- `protocol.c`：JSON Lines 编码、解析、socket 读写和发送锁。
- `document.c`：权威文档、操作模型、OT 位置变换、历史裁剪和重同步。
- `client_state.c`：客户端 `pending_queue`、`inflight_op`、ACK 和远程操作处理。
- `autosave.c`：保存进程、pipe 事件、原子写入、备份和 JSON Lines 日志。
- `server.c`：监听端口、接受连接、线程分发、ACK、广播和关闭保存。
- `client.c`：`ncursesw` 终端 UI、按键处理、后台网络读取线程。
- `events.c`：服务端终端事件格式化函数。
- `tests/test_termsync.c`：文档、协议、客户端状态和保存相关测试。

## 构建与验证

```bash
make
make test
./build/termsync_server --host 0.0.0.0 --port 8765 --file shared.c
./build/termsync_client --host glance02.xyz --port 8765 --client-id u1
```

编译参数使用 `gcc -std=c11 -Wall -Wextra -pedantic -D_XOPEN_SOURCE=700`，服务端和测试链接 `pthread`，客户端额外链接 `ncursesw`。
