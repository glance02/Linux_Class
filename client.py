"""协作编辑器的 curses 终端客户端。"""

from __future__ import annotations

import argparse
import locale
import queue
import socket
import threading
import unicodedata
from typing import List, Tuple, Union

try:
    import curses
except ModuleNotFoundError:
    curses = None  # type: ignore[assignment]

from client_state import ClientSyncState
from document import Operation
from protocol import JsonLineSocket, Message


CTRL_Q = 17
CTRL_S = 19
CONTROL_Q = "\x11"
CONTROL_S = "\x13"
IGNORED_INPUT = {"\t", "\r", "\x0b", "\x0c"}
KeyInput = Union[int, str]


def pos_to_line_col(content: str, pos: int) -> Tuple[int, int]:
    pos = max(0, min(pos, len(content)))
    line = content.count("\n", 0, pos)
    last_newline = content.rfind("\n", 0, pos)
    col = pos if last_newline < 0 else pos - last_newline - 1
    return line, col


def char_display_width(char: str) -> int:
    if not char:
        return 0
    if unicodedata.combining(char):
        return 0
    if unicodedata.east_asian_width(char) in {"F", "W"}:
        return 2
    return 1


def text_display_width(text: str) -> int:
    return sum(char_display_width(char) for char in text)


def display_col_to_char_col(text: str, display_col: int) -> int:
    display_col = max(0, display_col)
    current_col = 0
    for index, char in enumerate(text):
        width = char_display_width(char)
        if current_col + width > display_col:
            return index
        current_col += width
    return len(text)


def line_col_to_pos_by_display(content: str, line: int, display_col: int) -> int:
    lines = content.splitlines(keepends=True)
    if not lines:
        return 0
    line = max(0, min(line, len(lines) - 1))
    line_start = sum(len(part) for part in lines[:line])
    line_text = lines[line].rstrip("\n")
    return line_start + display_col_to_char_col(line_text, display_col)


def line_col_to_pos(content: str, line: int, col: int) -> int:
    lines = content.splitlines(keepends=True)
    if not lines:
        return 0
    line = max(0, min(line, len(lines) - 1))
    line_start = sum(len(part) for part in lines[:line])
    line_text = lines[line]
    visible_len = len(line_text.rstrip("\n"))
    return line_start + max(0, min(col, visible_len))


def split_lines_for_display(content: str) -> List[str]:
    lines = content.split("\n")
    return lines if lines else [""]


def is_insertable_character(value: str) -> bool:
    if len(value) != 1 or value in IGNORED_INPUT:
        return False
    return unicodedata.category(value)[0] != "C"


class TerminalClient:
    def __init__(self, host: str, port: int, client_id: str):
        self.host = host
        self.port = port
        self.client_id = client_id
        self.transport: JsonLineSocket
        self.state: ClientSyncState
        self.incoming: "queue.Queue[Message]" = queue.Queue()
        self.reader_thread: threading.Thread
        self.cursor_pos = 0
        self.scroll_line = 0
        self.running = True

    def connect(self) -> None:
        sock = socket.create_connection((self.host, self.port))
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.transport = JsonLineSocket(sock)
        self.state = ClientSyncState(self.client_id, self.transport.send_message)
        self.transport.send_message({"type": "JOIN", "client_id": self.client_id})
        self.reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self.reader_thread.start()

    def run(self) -> None:
        if curses is None:
            raise RuntimeError("client.py requires curses; please run it in Linux or WSL")
        self.connect()
        curses.wrapper(self._curses_main)

    def _reader_loop(self) -> None:
        while self.running:
            try:
                self.incoming.put(self.transport.recv_message())
            except Exception as exc:
                self.incoming.put({"type": "ERROR", "message": str(exc)})
                return

    def _curses_main(self, stdscr: "curses._CursesWindow") -> None:
        curses.curs_set(1)
        stdscr.nodelay(True)
        stdscr.keypad(True)
        while self.running:
            self._drain_messages()
            self._render(stdscr)
            try:
                key = stdscr.get_wch()
            except curses.error:
                curses.napms(25)
                continue
            self._handle_key(key)

    def _drain_messages(self) -> None:
        while True:
            try:
                message = self.incoming.get_nowait()
            except queue.Empty:
                return
            old_content = self.state.content
            self.state.handle_message(message)
            if self.state.content != old_content:
                self.cursor_pos = max(0, min(self.cursor_pos, len(self.state.content)))

    def _render(self, stdscr: "curses._CursesWindow") -> None:
        stdscr.erase()
        height, width = stdscr.getmaxyx()
        status = (
            f"{self.client_id} | v{self.state.version} | "
            f"pending={len(self.state.pending_queue)} | {self.state.status}"
        )
        stdscr.addnstr(0, 0, status, max(0, width - 1), curses.A_REVERSE)

        lines = split_lines_for_display(self.state.content)
        cursor_line, cursor_col = pos_to_line_col(self.state.content, self.cursor_pos)
        if cursor_line < self.scroll_line:
            self.scroll_line = cursor_line
        if cursor_line >= self.scroll_line + height - 2:
            self.scroll_line = cursor_line - height + 3

        for row in range(1, height - 1):
            line_index = self.scroll_line + row - 1
            if line_index >= len(lines):
                break
            prefix = f"{line_index + 1:>4} "
            body_width = max(0, width - len(prefix) - 1)
            stdscr.addnstr(row, 0, prefix, width - 1, curses.A_DIM)
            stdscr.addnstr(row, len(prefix), lines[line_index], body_width)

        help_text = "Ctrl-S save | Ctrl-Q quit"
        stdscr.addnstr(height - 1, 0, help_text, max(0, width - 1), curses.A_REVERSE)
        screen_row = cursor_line - self.scroll_line + 1
        current_line = lines[cursor_line] if cursor_line < len(lines) else ""
        screen_col = text_display_width(current_line[:cursor_col]) + 5
        if 1 <= screen_row < height - 1 and 0 <= screen_col < width:
            stdscr.move(screen_row, screen_col)
        stdscr.refresh()

    def _handle_key(self, key: KeyInput) -> None:
        if key in (CTRL_Q, CONTROL_Q):
            self.running = False
            return
        if key in (CTRL_S, CONTROL_S):
            self.transport.send_message({"type": "SAVE"})
            return
        if isinstance(key, str):
            if key == "\n":
                self.state.queue_operation(Operation("insert", self.cursor_pos, "\n"))
                self.cursor_pos += 1
                return
            if is_insertable_character(key):
                self.state.queue_operation(Operation("insert", self.cursor_pos, key))
                self.cursor_pos += 1
            return
        if key == curses.KEY_LEFT:
            self.cursor_pos = max(0, self.cursor_pos - 1)
            return
        if key == curses.KEY_RIGHT:
            self.cursor_pos = min(len(self.state.content), self.cursor_pos + 1)
            return
        if key == curses.KEY_UP:
            line, col = pos_to_line_col(self.state.content, self.cursor_pos)
            current_line = split_lines_for_display(self.state.content)[line]
            display_col = text_display_width(current_line[:col])
            self.cursor_pos = line_col_to_pos_by_display(self.state.content, line - 1, display_col)
            return
        if key == curses.KEY_DOWN:
            line, col = pos_to_line_col(self.state.content, self.cursor_pos)
            current_line = split_lines_for_display(self.state.content)[line]
            display_col = text_display_width(current_line[:col])
            self.cursor_pos = line_col_to_pos_by_display(self.state.content, line + 1, display_col)
            return
        if key in (curses.KEY_BACKSPACE, 127, 8):
            if self.cursor_pos > 0:
                delete_pos = self.cursor_pos - 1
                self.state.queue_operation(Operation("delete", delete_pos))
                self.cursor_pos = delete_pos
            return
        if key in (curses.KEY_ENTER, 10, 13):
            self.state.queue_operation(Operation("insert", self.cursor_pos, "\n"))
            self.cursor_pos += 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="终端协作编辑器客户端")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--client-id", default="u1")
    return parser.parse_args()


def main() -> None:
    locale.setlocale(locale.LC_ALL, "")
    args = parse_args()
    client = TerminalClient(args.host, args.port, args.client_id)
    client.run()


if __name__ == "__main__":
    main()
