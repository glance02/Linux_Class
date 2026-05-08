"""协作编辑器使用的 JSON Lines 协议辅助函数。

TCP 是字节流，不会天然保留“消息边界”。本项目把每条 JSON 消息后面加
一个换行符，形成 JSON Lines 协议，这样接收方只要读到换行就得到一条完整消息。
"""

from __future__ import annotations

import json
import socket
import threading
from typing import Any, Dict


Message = Dict[str, Any]
# 单条消息设置上限，避免异常客户端一直发送不带换行的大数据撑爆内存。
MAX_LINE_BYTES = 1024 * 1024


def encode_message(message: Message) -> bytes:
    """把一条 JSON 消息编码为 UTF-8 的 JSON Lines 数据帧。"""
    # ensure_ascii=False 让日志和协议里可以直接看到中文字符，
    # separators 去掉多余空格，减少网络传输字节数。
    return (
        json.dumps(message, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        + b"\n"
    )


def decode_message(line: bytes) -> Message:
    """把一条 JSON Lines 数据帧解码为消息字典。"""
    if not line:
        raise EOFError("connection closed")
    try:
        payload = json.loads(line.decode("utf-8"))
    except json.JSONDecodeError as exc:
        raise ValueError(f"invalid JSON message: {exc}") from exc
    if not isinstance(payload, dict):
        # 协议顶层必须是对象，方便所有消息都用 type 字段分发。
        raise ValueError("protocol message must be a JSON object")
    return payload


class JsonLineSocket:
    """对套接字做一层轻量封装，用于收发按换行分隔的 JSON 消息。"""

    def __init__(self, sock: socket.socket):
        self.sock = sock
        # recv 可能一次读到半条消息，也可能读到多条消息。
        # _buffer 保存尚未被 decode 的字节，直到找到换行符。
        self._buffer = bytearray()
        self._send_lock = threading.Lock()

    def recv_message(self) -> Message:
        while True:
            newline_index = self._buffer.find(b"\n")
            if newline_index >= 0:
                # 找到一条完整 JSON Lines 消息后，只消费到换行符为止；
                # 换行后面的字节保留给下一次 recv_message。
                line = bytes(self._buffer[:newline_index])
                del self._buffer[: newline_index + 1]
                return decode_message(line)

            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError("connection closed")
            self._buffer.extend(chunk)
            if len(self._buffer) > MAX_LINE_BYTES:
                raise ValueError("protocol message exceeds maximum line length")

    def send_message(self, message: Message) -> None:
        data = encode_message(message)
        # 同一个客户端连接可能被读线程、UI 线程或服务端广播逻辑同时使用。
        # 这里用发送锁保证一条 JSON Lines 消息会完整写入套接字，
        # 避免两次 sendall 的字节交叉在一起，导致对端 JSON 解析失败。
        with self._send_lock:
            self.sock.sendall(data)
