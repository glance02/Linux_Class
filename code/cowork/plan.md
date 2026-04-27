# 服务端主导 OT 的终端多人协作代码编辑器方案

## Summary
实现一个 Python 终端多人协作代码编辑器。服务端是唯一权威状态源，所有编辑操作进入文档处理临界区后才确定全局顺序；客户端只维护输入队列，保证连续按键不丢失，并按 ACK 节奏逐个发送。

## Key Changes
- 服务端全局排序：
  - 每个客户端连接由独立线程接收消息。
  - 编辑操作获取文档锁后进入临界区。
  - 服务端在锁内为操作分配递增 `server_version`。
  - `server_version` 就是该操作在全局历史中的顺序编号。
- 服务端主导 OT：
  - 客户端提交 `base_version + op`。
  - 若 `base_version` 落后于当前文档版本，服务端用历史操作把该操作变换到当前版本。
  - 服务端应用变换后的操作，写入历史，再生成待发送消息。
  - socket 广播必须在释放文档锁之后执行。
- 客户端发送节奏：
  - 客户端维护 `pending_queue` 和最多一个 `inflight_op`。
  - 用户连续按键时，操作先进入 `pending_queue`。
  - 当前一个操作收到 `ACK` 后，客户端再发送下一个操作。
  - 客户端本地文档以服务端 `ACK` / `REMOTE_OP` 为准，避免把复杂 OT 放到客户端。
- 历史操作裁剪：
  - 服务端使用有限长度历史记录，例如 `deque(maxlen=1000)`。
  - 维护 `history_start_version`。
  - 如果客户端提交的 `base_version < history_start_version`，服务端返回 `RESYNC_REQUIRED` 并发送完整 `DOC_STATE`。
  - 客户端收到完整文档后清空 `inflight_op`，保留或重建未发送的输入队列。

## Protocol And Flow
编辑操作消息：

```json
{"type":"OP","client_id":"u1","op_id":"u1-12","base_version":8,"op":{"kind":"insert","pos":5,"char":"a"}}
```

服务端处理流程：

1. 客户端线程收到 `OP`。
2. 获取文档锁。
3. 检查历史是否覆盖 `base_version`。
4. 根据 `base_version + 1` 到当前版本之间的历史操作执行 OT 变换。
5. 分配新的 `server_version`。
6. 应用变换后的操作到文档。
7. 记录历史操作。
8. 构造 `ACK` 和 `REMOTE_OP` 消息。
9. 释放文档锁。
10. 执行 socket 发送和广播。

客户端发送流程：

1. 用户输入转成 `insert(pos, char)` 或 `delete(pos)`。
2. 操作进入 `pending_queue`。
3. 如果没有 `inflight_op`，取队首发送。
4. 收到对应 `ACK` 后更新本地文档版本，清空 `inflight_op`。
5. 继续发送队列中的下一个操作。

## OT Rules
- 操作类型只支持字符级：
  - `insert(pos, char)`
  - `delete(pos)`
- 换行符 `\n` 作为普通字符处理。
- 粘贴拆成连续 `insert`。
- 替换拆成 `delete + insert`。
- 同位置并发插入：以进入服务端文档临界区的顺序为准，后进入的插入位置右移。
- 删除遇到之前插入：如果插入位置小于等于删除位置，删除位置右移。
- 插入遇到之前删除：如果删除位置小于插入位置，插入位置左移。
- 删除遇到之前删除：删除同一字符时后一个变为 no-op；如果之前删除位置更小，后一个删除位置左移。

## Test Plan
- 全局顺序测试：并发提交多个操作，检查 `server_version` 严格递增且与进入临界区顺序一致。
- OT 位置变换测试：A 在位置 5 插入 4 个字符后，B 基于旧版本删除位置 5，服务端将 B 的删除位置变换为 9。
- 客户端队列测试：连续快速输入多个字符，确认操作进入 `pending_queue`，并在 ACK 后逐个发送。
- 锁外广播测试：模拟慢客户端，确认服务端不会因 socket 发送阻塞文档锁。
- 历史裁剪测试：提交过旧 `base_version`，确认返回 `RESYNC_REQUIRED` 并重新发送完整文档。
- 文件系统测试：加载文件、手动保存、自动保存、备份文件和操作日志正确生成。

## Assumptions
- 使用 Python 标准库实现：`socket`、`threading`、`multiprocessing`、`queue`、`json`、`curses`、`pathlib`。
- 服务端是唯一权威状态源，客户端不实现复杂 OT。
- 默认每个客户端同一时间只允许一个未确认操作。
- 历史操作默认保留最近 1000 条。
- 项目目标是课程演示版，重点展示 Linux 综合编程能力和 OT 核心思想。
