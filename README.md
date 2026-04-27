# MiniCloud 终端多人协作代码编辑器

这是一个使用 Python 标准库实现的终端多人协作代码编辑器原型。项目采用“服务端主导 OT（Operational Transformation）”方案：客户端只负责收集用户输入并按 ACK 节奏发送，服务端作为唯一权威状态源，统一排序、变换、应用和广播编辑操作。

本项目适合作为 Linux 课程综合实验，能够覆盖进程、线程、线程同步、进程间通信、文件系统操作和网络编程等知识点。

## 功能特点

- 多个客户端通过 TCP 同时连接同一个服务端。
- 服务端为每个客户端连接创建独立线程。
- 所有编辑操作进入服务端文档临界区后，才分配递增的 `server_version`。
- `server_version` 同时表示操作在全局历史中的顺序编号。
- 服务端执行字符级 OT，支持：
  - `insert(pos, char)`：在指定位置插入单个字符。
  - `delete(pos)`：删除指定位置的单个字符。
- 客户端维护 `pending_queue`，连续按键不会丢失。
- 客户端同一时刻最多只有一个 `inflight_op`，收到 ACK 后再发送下一个操作。
- 服务端使用有限历史记录，历史过期时触发完整文档重同步。
- socket 广播在释放文档锁之后执行，避免慢客户端阻塞文档处理。
- 独立自动保存/日志进程通过 `multiprocessing.Queue` 与服务端通信。
- 支持文件加载、手动保存、自动保存、备份文件和编辑日志。

## 项目结构

```text
.
├── autosave.py          # 自动保存和日志进程
├── client.py            # curses 终端客户端
├── client_state.py      # 客户端 pending_queue / inflight_op 状态
├── document.py          # 服务端权威文档、OT、版本历史
├── protocol.py          # JSON Lines 网络协议封装
├── server.py            # TCP 多线程服务端
├── tests/               # 单元测试
└── README.md
```

## 运行方式

启动服务端：

```bash
python server.py --host 127.0.0.1 --port 8765 --file shared.py
```

打开两个终端，分别启动两个客户端：

```bash
python client.py --host 127.0.0.1 --port 8765 --client-id u1
python client.py --host 127.0.0.1 --port 8765 --client-id u2
```

客户端快捷键：

- `Ctrl-S`：请求服务端手动保存。
- `Ctrl-Q`：退出客户端。
- 方向键：移动光标。
- 普通字符：插入字符。
- Backspace：删除光标前一个字符。

说明：`client.py` 使用 Python 标准库 `curses`，建议在 Linux 或 WSL 终端中运行。

## 协议示例

客户端提交编辑操作：

```json
{"type":"OP","client_id":"u1","op_id":"u1-12","base_version":8,"op":{"kind":"insert","pos":5,"char":"a"}}
```

服务端确认操作：

```json
{"type":"ACK","client_id":"u1","op_id":"u1-12","server_version":9,"version":9,"op":{"kind":"insert","pos":5,"char":"a"}}
```

服务端向其他客户端广播：

```json
{"type":"REMOTE_OP","client_id":"u1","op_id":"u1-12","server_version":9,"version":9,"op":{"kind":"insert","pos":5,"char":"a"}}
```

历史过期时，服务端要求客户端重同步：

```json
{"type":"RESYNC_REQUIRED","reason":"operation history expired","server_version":120}
```

随后服务端发送完整文档：

```json
{"type":"DOC_STATE","version":120,"content":"..."}
```

## OT 核心思想

假设初始文本为：

```text
hello world
```

客户端 A 基于版本 0 在位置 5 连续插入 4 个 `!`，文本变成：

```text
hello!!!! world
```

客户端 B 仍然基于旧版本 0 请求删除位置 5 的字符。由于 A 已经在位置 5 前插入了 4 个字符，服务端会把 B 的删除位置从 5 变换为 9，再应用删除操作。

这就是本项目展示的 OT 核心：当一个旧版本操作到达服务端时，服务端会根据它之后已经发生的历史操作调整位置偏移。

## 测试

运行单元测试：

```bash
python -m unittest discover -s tests -v
```

测试覆盖内容：

- `server_version` 严格递增。
- 基于旧版本的操作经过 OT 后位置正确。
- 两个删除操作删除同一字符时，后一个变为 `noop`。
- 历史记录裁剪后触发 `RESYNC_REQUIRED`。
- 客户端连续输入进入 `pending_queue`，并在 ACK 后逐个发送。
- 手动保存会生成备份文件。

## 报告截图建议

- 服务端启动并监听端口。
- 两个客户端同时连接同一文档。
- 一个客户端输入，另一个客户端收到同步更新。
- OT 变换日志中展示变换前后的位置。
- 历史过期触发重同步。
- 手动保存、自动保存、备份文件和编辑日志生成结果。
