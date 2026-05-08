"""服务端权威协作编辑器的 TCP 服务端。

服务端是整个协作系统中唯一的“事实来源”：客户端只提交自己基于某个
版本产生的编辑意图，真正的排序、OT 变换、应用、广播和保存都在这里完成。
这样可以把复杂的并发冲突处理集中在服务端，客户端逻辑保持相对简单。
"""

from __future__ import annotations

import argparse
import multiprocessing
import os
import signal
import socket
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from autosave import autosave_worker
from document import CollaborativeDocument, ProcessResult
from protocol import JsonLineSocket, Message


def format_operation_event(entry: Message) -> str:
    # 把结构化日志事件转换成人能直接阅读的终端输出。
    # 这类格式化函数不参与协议，只是为了课堂演示时便于观察服务端行为。
    op = entry.get("transformed_op", {})
    kind = op.get("kind", "unknown") if isinstance(op, dict) else "unknown"
    pos = op.get("pos", "?") if isinstance(op, dict) else "?"
    message = (
        f"OP v{entry.get('server_version')} client={entry.get('client_id')} "
        f"{kind} pos={pos}"
    )
    if isinstance(op, dict) and kind == "insert":
        message += f" char={op.get('char')!r}"
    message += f" base={entry.get('base_version')}"
    return message


def format_server_event(entry: Message) -> Optional[str]:
    # 服务端日志里可能有多种事件。这里集中维护展示格式，避免业务逻辑里
    # 到处拼接字符串，也方便单元测试只验证格式化结果。
    event = entry.get("event")
    if event == "operation":
        return format_operation_event(entry)
    if event == "join":
        return f"JOIN client={entry.get('client_id')} address={entry.get('address')}"
    if event == "leave":
        return f"LEAVE client={entry.get('client_id')}"
    return None


def format_save_event(reason: str, client_id: Optional[str], version: object, path: Path) -> str:
    # 保存可能来自客户端请求，也可能来自服务端关闭时的最终保存。
    # client_id 为空时表示不是某个具体客户端触发的保存。
    if client_id:
        return f"SAVE {reason} client={client_id} version={version} file={path}"
    return f"SAVE {reason} version={version} file={path}"


@dataclass
class ClientConnection:
    """服务端保存的单个客户端连接信息。

    address 用于日志观察，transport 负责真正的 JSON Lines 收发。
    """

    client_id: str
    address: Tuple[str, int]
    transport: JsonLineSocket

    def send(self, message: Message) -> None:
        self.transport.send_message(message)


class CollaborativeServer:
    """多线程 TCP 服务端。

    每个客户端连接会分配一个独立线程；所有线程共享同一个
    CollaborativeDocument。文档内部自己持有锁，因此服务端这里主要负责
    管理客户端连接表、把消息分发到文档模型，以及把结果广播给其他客户端。
    """

    def __init__(
        self,
        host: str,
        port: int,
        document_path: Path,
        history_limit: int = 1000,
        autosave_interval: float = 5.0,
    ):
        self.host = host
        self.port = port
        self.document_path = document_path
        # multiprocessing 在 Windows 上可能重新导入模块并创建子进程。
        # 记录创建服务端对象的进程号，可以避免子进程误执行父进程的 shutdown。
        self._owner_pid = os.getpid()
        # 如果目标文件已经存在，就把它作为协作文档初始内容；否则从空文档开始。
        initial_text = document_path.read_text(encoding="utf-8") if document_path.exists() else ""
        self.document = CollaborativeDocument(initial_text, history_limit=history_limit)
        self.autosave_interval = autosave_interval
        # clients 字典会被多个客户端线程同时读写，所以用 RLock 保护。
        # 这个锁只保护“有哪些客户端在线”，不保护文档内容。
        self.clients: Dict[str, ClientConnection] = {}
        self.clients_lock = threading.RLock()
        self._next_client_number = 1
        self._stop_event = threading.Event()
        self._server_socket: Optional[socket.socket] = None
        # 自动保存放到独立进程中。主进程通过队列传递快照/日志事件，
        # 避免在网络线程或文档锁临界区内执行磁盘 I/O。
        self._autosave_queue: multiprocessing.Queue = multiprocessing.Queue()
        self._autosave_process = multiprocessing.Process(
            target=autosave_worker,
            args=(
                self._autosave_queue,
                str(document_path),
                str(document_path.with_name(document_path.name + ".edit.log")),
                autosave_interval,
            ),
            daemon=True,
        )

    def serve_forever(self) -> None:
        # 先启动保存进程，再把当前文档快照交给它。即使启动后暂时没有编辑，
        # autosave 进程也知道当前版本和内容。
        self._autosave_process.start()
        self._autosave_queue.put(
            {
                "type": "SNAPSHOT",
                "content": self.document.content(),
                "version": self.document.version,
            }
        )
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server_sock:
            self._server_socket = server_sock
            # SO_REUSEADDR 让服务端重启后能更快重新绑定同一个端口。
            server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server_sock.bind((self.host, self.port))
            server_sock.listen()
            # accept 设置短超时，循环中才能定期检查 _stop_event。
            server_sock.settimeout(0.5)
            bound_host, bound_port = server_sock.getsockname()
            print(f"Collaborative server listening on {bound_host}:{bound_port}", flush=True)
            print(f"Document: {self.document_path}", flush=True)

            while not self._stop_event.is_set():
                try:
                    client_sock, address = server_sock.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break
                # TCP_NODELAY 禁用 Nagle 算法，减少按键级小消息的交互延迟。
                client_sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                # 每个连接一个线程，适合课程中观察“多客户端并发连接”的模型。
                thread = threading.Thread(
                    target=self._handle_client,
                    args=(client_sock, address),
                    daemon=True,
                )
                thread.start()

        self.shutdown()

    def shutdown(self) -> None:
        # 子进程里如果持有一份反序列化出来的 server 对象，也不能关闭父进程资源。
        if os.getpid() != self._owner_pid:
            return
        # shutdown 可能由 Ctrl-C 信号、serve_forever 退出或测试重复触发，
        # 因此需要做成幂等操作。
        if self._stop_event.is_set():
            return
        self._stop_event.set()
        if self._server_socket is not None:
            try:
                self._server_socket.close()
            except OSError:
                pass
        self._autosave_queue.put(
            {
                "type": "SAVE_NOW",
                "content": self.document.content(),
                "version": self.document.version,
            }
        )
        self._print_event(format_save_event("shutdown", None, self.document.version, self.document_path))
        # STOP 要放在最终 SAVE_NOW 之后，确保保存进程先写完正式文件再退出。
        self._autosave_queue.put({"type": "STOP"})
        if self._autosave_process.is_alive():
            self._autosave_process.join(timeout=2.0)

    def _handle_client(self, sock: socket.socket, address: Tuple[str, int]) -> None:
        transport = JsonLineSocket(sock)
        client: Optional[ClientConnection] = None
        try:
            # 协议规定客户端第一条消息必须是 JOIN。这样服务端可以先确定
            # client_id，再发送 WELCOME 和当前完整文档快照。
            join_message = transport.recv_message()
            if join_message.get("type") != "JOIN":
                transport.send_message({"type": "ERROR", "message": "first message must be JOIN"})
                return
            client_id = self._choose_client_id(join_message.get("client_id"))
            client = ClientConnection(client_id=client_id, address=address, transport=transport)
            self._register_client(client)
            client.send({"type": "WELCOME", "client_id": client_id})
            # 新客户端必须先拿到完整 DOC_STATE，之后才能基于具体版本提交 OP。
            client.send(self.document.snapshot())
            self._log({"event": "join", "client_id": client_id, "address": str(address)})

            while not self._stop_event.is_set():
                # 客户端线程只负责持续收消息并转交给分发函数；
                # 具体业务处理集中在 _dispatch_message 中。
                message = transport.recv_message()
                self._dispatch_message(client, message)
        except EOFError:
            pass
        except Exception as exc:
            if client is not None:
                self._safe_send(client, {"type": "ERROR", "message": str(exc)})
        finally:
            # 无论是正常断开、EOF 还是异常，都要把客户端从在线表移除。
            if client is not None:
                self._unregister_client(client.client_id)
                self._log({"event": "leave", "client_id": client.client_id})
            try:
                sock.close()
            except OSError:
                pass

    def _dispatch_message(self, client: ClientConnection, message: Message) -> None:
        # 服务端协议是按 type 字段分发的。未知消息不会断开连接，
        # 而是返回 ERROR，方便客户端或测试观察问题。
        message_type = message.get("type")
        if message_type == "OP":
            self._handle_operation(client, message)
        elif message_type == "SAVE":
            # SAVE 不直接在客户端线程写文件，而是把当前快照投递给保存进程。
            # 客户端收到 SAVE_QUEUED 表示请求已经进入保存队列。
            snapshot = self.document.snapshot()
            self._autosave_queue.put(
                {
                    "type": "SAVE_NOW",
                    "content": snapshot["content"],
                    "version": snapshot["version"],
                }
            )
            self._print_event(
                format_save_event("requested", client.client_id, snapshot["version"], self.document_path)
            )
            client.send({"type": "SAVE_QUEUED", "version": snapshot["version"]})
        elif message_type == "REQUEST_DOC":
            # 客户端发现版本断档或服务端要求重同步时，会主动请求完整文档。
            client.send(self.document.snapshot())
        elif message_type == "PING":
            client.send({"type": "PONG"})
        else:
            client.send({"type": "ERROR", "message": f"unsupported message type: {message_type!r}"})

    def _handle_operation(self, client: ClientConnection, message: Message) -> None:
        # process_operation 内部会获取文档锁，并在临界区内完成 OT、
        # server_version 分配和文档更新。函数返回后，文档锁已经释放。
        result = self.document.process_operation(
            client_id=client.client_id,
            op_id=str(message.get("op_id", "")),
            base_version=message.get("base_version", -1),
            op_payload=message.get("op", {}),
        )

        if result.status == "ok":
            self._record_successful_operation(result)
            # 注意：ACK 和 REMOTE_OP 都在文档锁外发送。
            # 这样即使某个客户端网络很慢，也不会阻塞其他线程进入文档临界区。
            if result.ack is not None:
                client.send(result.ack)
            if result.remote is not None:
                self._broadcast(result.remote, exclude_client_id=client.client_id)
            return

        for response in result.messages:
            client.send(response)

    def _record_successful_operation(self, result: ProcessResult) -> None:
        # 文档模型返回的 ProcessResult 已经把“要保存的快照”和“要写的日志”
        # 与网络 ACK/广播消息分开，服务端在这里分别投递到对应通道。
        if result.snapshot_content is not None and result.snapshot_version is not None:
            self._autosave_queue.put(
                {
                    "type": "SNAPSHOT",
                    "content": result.snapshot_content,
                    "version": result.snapshot_version,
                }
            )
        if result.log_event is not None:
            self._log(result.log_event)

    def _choose_client_id(self, requested: object) -> str:
        # 如果客户端请求的 id 可用就保留，方便演示时指定 u1/u2。
        # 冲突或空 id 则由服务端分配递增编号。
        with self.clients_lock:
            if isinstance(requested, str) and requested and requested not in self.clients:
                return requested
            while True:
                candidate = f"u{self._next_client_number}"
                self._next_client_number += 1
                if candidate not in self.clients:
                    return candidate

    def _register_client(self, client: ClientConnection) -> None:
        # 在线客户端表是广播的收件人来源，必须在锁内更新。
        with self.clients_lock:
            self.clients[client.client_id] = client

    def _unregister_client(self, client_id: str) -> None:
        # 客户端可能已经因为发送失败被提前移除，所以 pop 使用默认值保持幂等。
        with self.clients_lock:
            self.clients.pop(client_id, None)

    def _broadcast(self, message: Message, exclude_client_id: Optional[str] = None) -> None:
        # 持有客户端注册表锁时只复制收件人列表，释放锁之后再执行套接字 I/O。
        # 这里的锁只保护 clients 字典，不保护文档内容。复制收件人列表后立刻释放，
        # 可以避免广播阶段和客户端加入/退出管理互相长时间阻塞。
        with self.clients_lock:
            recipients: List[ClientConnection] = [
                client
                for cid, client in self.clients.items()
                if cid != exclude_client_id
            ]
        failed: List[str] = []
        for recipient in recipients:
            if not self._safe_send(recipient, message):
                failed.append(recipient.client_id)
        for client_id in failed:
            self._unregister_client(client_id)

    def _safe_send(self, client: ClientConnection, message: Message) -> bool:
        # 广播时某个客户端断开不应该影响其他客户端，因此发送失败只返回 False。
        try:
            client.send(message)
            return True
        except OSError:
            return False

    def _log(self, entry: Message) -> None:
        # 同一份结构化事件既进入磁盘日志，也转换为终端输出。
        self._autosave_queue.put({"type": "LOG", "entry": entry})
        self._print_event(format_server_event(entry))

    def _print_event(self, message: Optional[str]) -> None:
        if message:
            print(message, flush=True)


def parse_args() -> argparse.Namespace:
    # 命令行参数保持简单，便于在 Linux/WSL 终端中直接启动服务端演示。
    parser = argparse.ArgumentParser(description="服务端权威 OT 协作编辑器")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--file", default="shared.py", help="要协作编辑的文档路径")
    parser.add_argument("--history", type=int, default=1000)
    parser.add_argument("--autosave-interval", type=float, default=5.0)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    server = CollaborativeServer(
        host=args.host,
        port=args.port,
        document_path=Path(args.file),
        history_limit=args.history,
        autosave_interval=args.autosave_interval,
    )

    def stop(_signum: int, _frame: object) -> None:
        # Ctrl-C / SIGTERM 触发正常收尾：最终保存、通知 autosave 进程退出。
        server.shutdown()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    server.serve_forever()


if __name__ == "__main__":
    main()
