"""自动保存和编辑日志工作进程。"""

from __future__ import annotations

import json
import shutil
import time
from multiprocessing import Queue
from pathlib import Path
from queue import Empty
from typing import Any, Dict, Optional


def atomic_write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = path.with_name(path.name + ".tmp")
    tmp_path.write_text(content, encoding="utf-8")
    tmp_path.replace(path)


def write_with_backup(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        backup_path = path.with_name(path.name + ".bak")
        shutil.copy2(path, backup_path)
    atomic_write_text(path, content)


def append_json_log(path: Path, entry: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(entry, ensure_ascii=False, sort_keys=True) + "\n")


def autosave_worker(
    event_queue: Queue,
    document_path: str,
    log_path: str,
    interval_seconds: float = 5.0,
) -> None:
    # 自动保存和日志写入放在单独进程中完成。
    # 主服务端只需要通过 multiprocessing.Queue 投递事件，不必在处理
    # 网络请求或文档锁时直接做磁盘 I/O，从而体现进程间通信的使用。
    target_path = Path(document_path)
    autosave_path = target_path.with_name(target_path.name + ".autosave")
    log_file = Path(log_path)
    latest_content: Optional[str] = None
    latest_version = 0
    next_save = time.monotonic() + interval_seconds

    while True:
        timeout = max(0.0, next_save - time.monotonic())
        try:
            event = event_queue.get(timeout=timeout)
        except Empty:
            event = None

        if event:
            event_type = event.get("type")
            if event_type == "STOP":
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
                entry = dict(event.get("entry", {}))
                entry.setdefault("timestamp", time.time())
                append_json_log(log_file, entry)

        if time.monotonic() >= next_save:
            if latest_content is not None:
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
