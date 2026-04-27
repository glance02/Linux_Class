"""客户端待发送队列和权威状态应用逻辑。"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass
from typing import Callable, Deque, Dict, Optional

from document import Operation, apply_operation_to_text
from protocol import Message


@dataclass
class PendingOperation:
    op_id: str
    op: Operation
    base_version: Optional[int] = None


class ClientSyncState:
    """缓存本地输入，并保证同一时刻最多只有一个未确认操作。"""

    def __init__(self, client_id: str, send_message: Callable[[Message], None]):
        self.client_id = client_id
        self.send_message = send_message
        self.content = ""
        self.version = 0
        self.history_start_version = 0
        self.pending_queue: Deque[PendingOperation] = deque()
        self.inflight_op: Optional[PendingOperation] = None
        self._next_op_number = 1
        self.status = "disconnected"

    def set_document(self, content: str, version: int, history_start_version: int = 0) -> None:
        self.content = content
        self.version = version
        self.history_start_version = history_start_version
        self.inflight_op = None
        self.status = f"synced at version {version}"
        self._pump()

    def queue_operation(self, op: Operation) -> str:
        op_id = f"{self.client_id}-{self._next_op_number}"
        self._next_op_number += 1
        # 用户连续按键时不能因为上一个操作还没 ACK 就丢输入。
        # 所有本地输入先进入 pending_queue，再由 _pump 按顺序发送。
        self.pending_queue.append(PendingOperation(op_id=op_id, op=op))
        self.status = f"queued {len(self.pending_queue)} operation(s)"
        self._pump()
        return op_id

    def handle_message(self, message: Message) -> None:
        message_type = message.get("type")
        if message_type == "DOC_STATE":
            self.set_document(
                content=str(message.get("content", "")),
                version=int(message.get("version", 0)),
                history_start_version=int(message.get("history_start_version", 0)),
            )
        elif message_type == "WELCOME":
            self.client_id = str(message.get("client_id", self.client_id))
            self.status = f"connected as {self.client_id}"
        elif message_type == "ACK":
            self._handle_ack(message)
        elif message_type == "REMOTE_OP":
            self._handle_remote_op(message)
        elif message_type == "RESYNC_REQUIRED":
            self.status = "server requested resync"
            self.inflight_op = None
            self.send_message({"type": "REQUEST_DOC"})
        elif message_type == "SAVE_QUEUED":
            self.status = f"save queued at version {message.get('version')}"
        elif message_type == "ERROR":
            self.status = f"error: {message.get('message')}"

    def _handle_ack(self, message: Message) -> None:
        op_id = str(message.get("op_id", ""))
        if self.inflight_op is None or self.inflight_op.op_id != op_id:
            self.status = f"ignored stale ACK {op_id}"
            return
        # 客户端不自行决定本地操作最终落点，而是等待服务端 ACK 中的
        # 权威 op。这个 op 可能已经被服务端 OT 变换过。
        self._apply_authoritative_op(message)
        self.inflight_op = None
        self.status = f"ACK {op_id} at version {self.version}"
        self._pump()

    def _handle_remote_op(self, message: Message) -> None:
        incoming_version = int(message.get("version", 0))
        if incoming_version <= self.version:
            return
        if incoming_version != self.version + 1:
            self.status = "missed remote operation; requesting document"
            self.send_message({"type": "REQUEST_DOC"})
            return
        self._apply_authoritative_op(message)
        self.status = f"remote op at version {self.version}"

    def _apply_authoritative_op(self, message: Message) -> None:
        op = Operation.from_dict(message.get("op", {"kind": "noop"}))
        self.content = apply_operation_to_text(self.content, op)
        self.version = int(message.get("version", self.version))

    def _pump(self) -> None:
        if self.inflight_op is not None or not self.pending_queue:
            return
        next_op = self.pending_queue.popleft()
        next_op.base_version = self.version
        self.inflight_op = next_op
        # 每个客户端同一时刻只发送一个未确认操作。
        # 这样客户端侧不需要实现复杂 OT，只要记住当前版本并等待 ACK。
        self.send_message(
            {
                "type": "OP",
                "client_id": self.client_id,
                "op_id": next_op.op_id,
                "base_version": next_op.base_version,
                "op": next_op.op.to_dict(),
            }
        )
