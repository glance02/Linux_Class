"""带有服务端 OT 和有限历史记录的权威文档模型。"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field
from threading import RLock
from typing import Any, Deque, Dict, Iterable, List, Optional


VALID_KINDS = {"insert", "delete", "noop"}


@dataclass
class Operation:
    kind: str
    pos: int = 0
    char: Optional[str] = None
    reason: Optional[str] = None

    @classmethod
    def from_dict(cls, payload: Dict[str, Any]) -> "Operation":
        if not isinstance(payload, dict):
            raise ValueError("operation must be a JSON object")
        kind = payload.get("kind")
        if kind not in VALID_KINDS:
            raise ValueError(f"unsupported operation kind: {kind!r}")
        pos = payload.get("pos", 0)
        if not isinstance(pos, int):
            raise ValueError("operation pos must be an integer")
        char = payload.get("char")
        if kind == "insert":
            if not isinstance(char, str) or len(char) != 1:
                raise ValueError("insert operation requires a single-character char")
        else:
            char = None
        reason = payload.get("reason")
        if reason is not None and not isinstance(reason, str):
            reason = str(reason)
        return cls(kind=kind, pos=pos, char=char, reason=reason)

    def clone(self) -> "Operation":
        return Operation(self.kind, self.pos, self.char, self.reason)

    def to_dict(self) -> Dict[str, Any]:
        payload: Dict[str, Any] = {"kind": self.kind, "pos": self.pos}
        if self.char is not None:
            payload["char"] = self.char
        if self.reason:
            payload["reason"] = self.reason
        return payload

    def as_noop(self, reason: str) -> "Operation":
        return Operation("noop", self.pos, None, reason)


@dataclass
class HistoryEntry:
    server_version: int
    client_id: str
    op_id: str
    base_version: int
    original_op: Operation
    op: Operation

    def to_dict(self) -> Dict[str, Any]:
        return {
            "server_version": self.server_version,
            "client_id": self.client_id,
            "op_id": self.op_id,
            "base_version": self.base_version,
            "original_op": self.original_op.to_dict(),
            "op": self.op.to_dict(),
        }


@dataclass
class ProcessResult:
    status: str
    messages: List[Dict[str, Any]] = field(default_factory=list)
    ack: Optional[Dict[str, Any]] = None
    remote: Optional[Dict[str, Any]] = None
    log_event: Optional[Dict[str, Any]] = None
    snapshot_content: Optional[str] = None
    snapshot_version: Optional[int] = None


def apply_operation_to_text(text: str, op: Operation) -> str:
    """把已经规范化的位置操作应用到文本上。"""
    if op.kind == "noop":
        return text
    if op.kind == "insert":
        pos = max(0, min(op.pos, len(text)))
        return text[:pos] + (op.char or "") + text[pos:]
    if op.kind == "delete":
        if 0 <= op.pos < len(text):
            return text[: op.pos] + text[op.pos + 1 :]
        return text
    raise ValueError(f"unsupported operation kind: {op.kind!r}")


def transform_against(op: Operation, previous: Operation) -> Operation:
    """根据一条已经发生的历史操作，变换当前操作的位置。"""
    result = op.clone()
    if result.kind == "noop" or previous.kind == "noop":
        return result

    if previous.kind == "insert":
        # 历史中已经发生过一次插入，会把它后面的字符整体向右推一格。
        # 因此当前操作如果作用在插入点之后，或者正好作用在同一位置，
        # 都需要把位置加 1，才能继续指向原来想编辑的字符。
        if result.kind == "insert" and previous.pos <= result.pos:
            result.pos += 1
        elif result.kind == "delete" and previous.pos <= result.pos:
            result.pos += 1
        return result

    if previous.kind == "delete":
        # 历史中已经发生过一次删除，会把它后面的字符整体向左拉一格。
        # 插入操作只在历史删除点严格位于自己之前时左移；如果同位置插入，
        # 插入仍然发生在这个位置。删除操作如果删除同一个字符，则变成 noop。
        if result.kind == "insert" and previous.pos < result.pos:
            result.pos -= 1
        elif result.kind == "delete":
            if previous.pos < result.pos:
                result.pos -= 1
            elif previous.pos == result.pos:
                return result.as_noop("same character already deleted")
        return result

    raise ValueError(f"unsupported history operation kind: {previous.kind!r}")


class CollaborativeDocument:
    """线程安全的服务端权威文档状态。

    编辑操作只有进入当前对象的锁保护临界区后，才会获得服务端版本号。
    这个版本号也就是该操作在有限历史记录中的全局顺序编号。
    """

    def __init__(self, initial_text: str = "", history_limit: int = 1000):
        if history_limit <= 0:
            raise ValueError("history_limit must be positive")
        self._text = initial_text
        self.version = 0
        self.history: Deque[HistoryEntry] = deque(maxlen=history_limit)
        self.history_start_version = 0
        self._lock = RLock()

    def snapshot(self) -> Dict[str, Any]:
        with self._lock:
            return {
                "type": "DOC_STATE",
                "version": self.version,
                "history_start_version": self.history_start_version,
                "content": self._text,
            }

    def content(self) -> str:
        with self._lock:
            return self._text

    def process_operation(
        self,
        client_id: str,
        op_id: str,
        base_version: int,
        op_payload: Dict[str, Any],
    ) -> ProcessResult:
        try:
            original_op = Operation.from_dict(op_payload)
        except ValueError as exc:
            return ProcessResult(
                status="error",
                messages=[{"type": "ERROR", "op_id": op_id, "message": str(exc)}],
            )

        with self._lock:
            # 下面这段就是服务端的“文档处理临界区”：
            # 1. 检查客户端版本是否还能被历史覆盖；
            # 2. 对旧版本操作做 OT 位置变换；
            # 3. 分配新的 server_version；
            # 4. 应用操作并写入有限历史。
            # 套接字发送不在这里做，避免慢客户端长期占用文档锁。
            if not isinstance(base_version, int):
                return ProcessResult(
                    status="error",
                    messages=[
                        {
                            "type": "ERROR",
                            "op_id": op_id,
                            "message": "base_version must be an integer",
                        }
                    ],
                )

            if base_version < self.history_start_version or base_version > self.version:
                return ProcessResult(
                    status="resync",
                    messages=[
                        {
                            "type": "RESYNC_REQUIRED",
                            "op_id": op_id,
                            "reason": "operation history expired"
                            if base_version < self.history_start_version
                            else "client version is ahead of server",
                            "server_version": self.version,
                            "history_start_version": self.history_start_version,
                        },
                        self._snapshot_unlocked(),
                    ],
                )

            transformed = original_op.clone()
            transform_trace: List[Dict[str, Any]] = []
            # 客户端提交的 base_version 可能落后于服务端当前版本。
            # 这里把该版本之后的每一条权威历史操作依次拿出来，
            # 将客户端操作一步步变换到“现在”可以安全应用的位置。
            for entry in self._history_after_unlocked(base_version):
                before = transformed.to_dict()
                transformed = transform_against(transformed, entry.op)
                after = transformed.to_dict()
                if before != after:
                    transform_trace.append(
                        {
                            "against_version": entry.server_version,
                            "against_op": entry.op.to_dict(),
                            "before": before,
                            "after": after,
                        }
                    )

            transformed = self._normalize_for_apply_unlocked(transformed)
            # 操作只有进入临界区并完成 OT 后才获得 server_version。
            # 这个递增编号就是全局操作顺序，不依赖客户端时间戳或网络发送顺序。
            self.version += 1
            server_version = self.version
            self._text = apply_operation_to_text(self._text, transformed)

            history_entry = HistoryEntry(
                server_version=server_version,
                client_id=client_id,
                op_id=op_id,
                base_version=base_version,
                original_op=original_op,
                op=transformed,
            )
            self.history.append(history_entry)
            self._refresh_history_start_unlocked()

            ack = {
                "type": "ACK",
                "client_id": client_id,
                "op_id": op_id,
                "base_version": base_version,
                "server_version": server_version,
                "version": self.version,
                "op": transformed.to_dict(),
                "original_op": original_op.to_dict(),
            }
            remote = {
                "type": "REMOTE_OP",
                "client_id": client_id,
                "op_id": op_id,
                "server_version": server_version,
                "version": self.version,
                "op": transformed.to_dict(),
            }
            log_event = {
                "event": "operation",
                "server_version": server_version,
                "client_id": client_id,
                "op_id": op_id,
                "base_version": base_version,
                "original_op": original_op.to_dict(),
                "transformed_op": transformed.to_dict(),
                "transform_trace": transform_trace,
                "content_length": len(self._text),
            }
            return ProcessResult(
                status="ok",
                ack=ack,
                remote=remote,
                log_event=log_event,
                snapshot_content=self._text,
                snapshot_version=self.version,
            )

    def _snapshot_unlocked(self) -> Dict[str, Any]:
        return {
            "type": "DOC_STATE",
            "version": self.version,
            "history_start_version": self.history_start_version,
            "content": self._text,
        }

    def _history_after_unlocked(self, base_version: int) -> Iterable[HistoryEntry]:
        for entry in self.history:
            if entry.server_version > base_version:
                yield entry

    def _normalize_for_apply_unlocked(self, op: Operation) -> Operation:
        if op.kind == "noop":
            return op
        if op.kind == "insert":
            pos = max(0, min(op.pos, len(self._text)))
            return Operation("insert", pos, op.char)
        if op.kind == "delete":
            if 0 <= op.pos < len(self._text):
                return Operation("delete", op.pos)
            return op.as_noop("delete position out of range")
        raise ValueError(f"unsupported operation kind: {op.kind!r}")

    def _refresh_history_start_unlocked(self) -> None:
        if not self.history:
            self.history_start_version = self.version
            return
        # 如果当前保留的最早历史操作版本是 N + 1，那么客户端从 base N
        # 开始仍然可以做 OT 变换；更旧的版本就必须重同步完整文档。
        # deque(maxlen=N) 会自动丢弃旧历史，所以这里记录最早还能支持
        # OT 变换的 base_version。客户端再旧就不能猜测位置，只能重同步全文。
        self.history_start_version = max(0, self.history[0].server_version - 1)
