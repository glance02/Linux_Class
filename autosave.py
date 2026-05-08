"""自动保存和编辑日志工作进程。

服务端把磁盘写入工作交给这个模块中的 worker 进程完成。这样主服务端线程
处理网络请求和文档锁时，不会因为文件系统 I/O 变慢而卡住其他客户端。
"""

from __future__ import annotations

import json
import signal
import shutil
import time
from multiprocessing import Queue
from pathlib import Path
from queue import Empty
from typing import Any, Dict, Optional


def atomic_write_text(path: Path, content: str) -> None:
    # 先写到临时文件，再用 replace 一次性替换目标文件。
    # 这样即使程序在写入中途崩溃，也尽量避免留下半截正式文件。
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = path.with_name(path.name + ".tmp")
    tmp_path.write_text(content, encoding="utf-8")
    tmp_path.replace(path)


def write_with_backup(path: Path, content: str) -> None:
    # 手动保存会覆盖正式文件，所以覆盖前先复制一份 .bak。
    # 自动保存只写 .autosave，不走这里。
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        backup_path = path.with_name(path.name + ".bak")
        shutil.copy2(path, backup_path)
    atomic_write_text(path, content)


def append_json_log(path: Path, entry: Dict[str, Any]) -> None:
    # 日志使用 JSON Lines：每一行是一个独立 JSON 对象，便于 tail 查看，
    # 也便于后续用脚本逐行分析。
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(entry, ensure_ascii=False, sort_keys=True) + "\n")


def autosave_worker(
    event_queue: Queue,
    document_path: str,
    log_path: str,
    interval_seconds: float = 5.0,
) -> None:
    # 保存进程不响应 Ctrl-C，由父进程负责发送 STOP 做有序退出。
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    # 自动保存和日志写入放在单独进程中完成。
    # 主服务端只需要通过 multiprocessing.Queue 投递事件，不必在处理
    # 网络请求或文档锁时直接做磁盘 I/O，从而体现进程间通信的使用。
    target_path = Path(document_path)
    autosave_path = target_path.with_name(target_path.name + ".autosave")
    log_file = Path(log_path)
    latest_content: Optional[str] = None
    latest_version = 0
    # 使用 monotonic 计算间隔，避免系统时间被手动调整影响自动保存节奏。
    next_save = time.monotonic() + interval_seconds

    while True:
        # 队列 get 的超时时间正好是“距离下一次自动保存还剩多久”。
        # 如果超时没有新事件，就执行一次定时保存检查。
        timeout = max(0.0, next_save - time.monotonic())
        try:
            event = event_queue.get(timeout=timeout)
        except Empty:
            event = None

        if event:
            event_type = event.get("type")
            if event_type == "STOP":
                # 退出前把最后一份快照写入 autosave 文件，减少关闭时的数据丢失窗口。
                if latest_content is not None:
                    atomic_write_text(autosave_path, latest_content)
                return
            if event_type == "SNAPSHOT":
                # 服务端每处理完一次成功编辑，就发送最新快照。
                # 工作进程只保存最后一次快照，定时落盘，避免每个按键都写文件。
                latest_content = event.get("content", "")
                latest_version = int(event.get("version", latest_version))
            elif event_type == "SAVE_NOW":
                # 手动保存写入正式文件，并在覆盖前生成 .bak 备份。
                content = event.get("content", latest_content or "")
                latest_content = content
                latest_version = int(event.get("version", latest_version))
                write_with_backup(target_path, content)
                append_json_log(
                    log_file,
                    {
                        "event": "manual_save",
                        "version": latest_version,
                        "path": str(target_path),
                        "timestamp": time.time(),
                    },
                )
            elif event_type == "LOG":
                # 普通编辑日志不改文档文件，只追加一行 JSON。
                entry = dict(event.get("entry", {}))
                entry.setdefault("timestamp", time.time())
                append_json_log(log_file, entry)

        if time.monotonic() >= next_save:
            if latest_content is not None:
                # 自动保存写的是旁路快照文件，不覆盖正式文件。
                atomic_write_text(autosave_path, latest_content)
                append_json_log(
                    log_file,
                    {
                        "event": "autosave",
                        "version": latest_version,
                        "path": str(autosave_path),
                        "timestamp": time.time(),
                    },
                )
            next_save = time.monotonic() + interval_seconds
