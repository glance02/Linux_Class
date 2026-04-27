"""服务端权威协作编辑器的 TCP 服务端。"""

from __future__ import annotations

import argparse
import multiprocessing
import signal
import socket
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from autosave import autosave_worker
from document import CollaborativeDocument, ProcessResult
from protocol import JsonLineSocket, Message


@dataclass
class ClientConnection:
    client_id: str
    address: Tuple[str, int]
    transport: JsonLineSocket

    def send(self, message: Message) -> None:
        self.transport.send_message(message)


class CollaborativeServer:
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
        initial_text = document_path.read_text(encoding="utf-8") if document_path.exists() else ""
        self.document = CollaborativeDocument(initial_text, history_limit=history_limit)
        self.autosave_interval = autosave_interval
        self.clients: Dict[str, ClientConnection] = {}
        self.clients_lock = threading.RLock()
        self._next_client_number = 1
        self._stop_event = threading.Event()
        self._server_socket: Optional[socket.socket] = None
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
            server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server_sock.bind((self.host, self.port))
            server_sock.listen()
            server_sock.settimeout(0.5)
            bound_host, bound_port = server_sock.getsockname()
            print(f"Collaborative server listening on {bound_host}:{bound_port}")
            print(f"Document: {self.document_path}")

            while not self._stop_event.is_set():
                try:
                    client_sock, address = server_sock.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break
                client_sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                thread = threading.Thread(
                    target=self._handle_client,
                    args=(client_sock, address),
                    daemon=True,
                )
                thread.start()

        self.shutdown()

    def shutdown(self) -> None:
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
        self._autosave_queue.put({"type": "STOP"})
        if self._autosave_process.is_alive():
            self._autosave_process.join(timeout=2.0)

    def _handle_client(self, sock: socket.socket, address: Tuple[str, int]) -> None:
        transport = JsonLineSocket(sock)
        client: Optional[ClientConnection] = None
        try:
            join_message = transport.recv_message()
            if join_message.get("type") != "JOIN":
                transport.send_message({"type": "ERROR", "message": "first message must be JOIN"})
                return
            client_id = self._choose_client_id(join_message.get("client_id"))
            client = ClientConnection(client_id=client_id, address=address, transport=transport)
            self._register_client(client)
            client.send({"type": "WELCOME", "client_id": client_id})
            client.send(self.document.snapshot())
            self._log({"event": "join", "client_id": client_id, "address": str(address)})

            while not self._stop_event.is_set():
                message = transport.recv_message()
                self._dispatch_message(client, message)
        except EOFError:
            pass
        except Exception as exc:
            if client is not None:
                self._safe_send(client, {"type": "ERROR", "message": str(exc)})
        finally:
            if client is not None:
                self._unregister_client(client.client_id)
                self._log({"event": "leave", "client_id": client.client_id})
            try:
                sock.close()
            except OSError:
                pass

    def _dispatch_message(self, client: ClientConnection, message: Message) -> None:
        message_type = message.get("type")
        if message_type == "OP":
            self._handle_operation(client, message)
        elif message_type == "SAVE":
            snapshot = self.document.snapshot()
            self._autosave_queue.put(
                {
                    "type": "SAVE_NOW",
                    "content": snapshot["content"],
                    "version": snapshot["version"],
                }
            )
            client.send({"type": "SAVE_QUEUED", "version": snapshot["version"]})
        elif message_type == "REQUEST_DOC":
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
        with self.clients_lock:
            if isinstance(requested, str) and requested and requested not in self.clients:
                return requested
            while True:
                candidate = f"u{self._next_client_number}"
                self._next_client_number += 1
                if candidate not in self.clients:
                    return candidate

    def _register_client(self, client: ClientConnection) -> None:
        with self.clients_lock:
            self.clients[client.client_id] = client

    def _unregister_client(self, client_id: str) -> None:
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
        try:
            client.send(message)
            return True
        except OSError:
            return False

    def _log(self, entry: Message) -> None:
        self._autosave_queue.put({"type": "LOG", "entry": entry})


def parse_args() -> argparse.Namespace:
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
        server.shutdown()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    server.serve_forever()


if __name__ == "__main__":
    main()
