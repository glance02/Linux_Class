"""协作编辑器的 curses 终端客户端。

客户端负责把用户在终端中的按键转换为字符级 Operation，并把服务端返回的
权威文档状态渲染出来。它不做复杂冲突解决，冲突处理统一交给服务端 OT。
"""

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
    # Windows 原生 Python 通常没有 curses；WSL/Linux 终端中可以正常运行。
    curses = None  # type: ignore[assignment]

from client_state import ClientSyncState
from document import Operation
from protocol import JsonLineSocket, Message


CTRL_Q = 17
CTRL_S = 19
CONTROL_Q = "\x11"
CONTROL_S = "\x13"
# 不同终端对 Backspace 的编码可能不同，因此同时接受常见形式。
BACKSPACE_KEYS = {"\b", "\x7f"}
# 这些控制字符在本编辑器里没有定义行为，避免误当作普通字符插入。
IGNORED_INPUT = {"\t", "\r", "\x0b", "\x0c"}
KeyInput = Union[int, str]


def pos_to_line_col(content: str, pos: int) -> Tuple[int, int]:
    # 文档内部用“从头开始的字符索引”表示光标位置；
    # curses 渲染时需要转换成第几行、第几列。
    pos = max(0, min(pos, len(content)))
    line = content.count("\n", 0, pos)
    last_newline = content.rfind("\n", 0, pos)
    col = pos if last_newline < 0 else pos - last_newline - 1
    return line, col


def char_display_width(char: str) -> int:
    # len("你") 是 1，但在大多数终端里占 2 个显示列。
    # curses move 需要的是屏幕列，因此要单独计算显示宽度。
    if not char:
        return 0
    if unicodedata.combining(char):
        return 0
    if unicodedata.east_asian_width(char) in {"F", "W"}:
        return 2
    return 1


def text_display_width(text: str) -> int:
    # 逐字符累加显示宽度，支持中英文混排时的光标定位。
    return sum(char_display_width(char) for char in text)


def display_col_to_char_col(text: str, display_col: int) -> int:
    # 把屏幕显示列转换回字符串字符索引。
    # 向上/向下移动时，希望尽量保持“视觉列”不变，而不是简单保持字符数。
    display_col = max(0, display_col)
    current_col = 0
    for index, char in enumerate(text):
        width = char_display_width(char)
        if current_col + width > display_col:
            return index
        current_col += width
    return len(text)


def line_col_to_pos_by_display(content: str, line: int, display_col: int) -> int:
    # 根据目标行和视觉列找到文档中的一维字符索引。
    # 主要用于上下方向键处理宽字符时的光标位置。
    lines = content.splitlines(keepends=True)
    if not lines:
        return 0
    line = max(0, min(line, len(lines) - 1))
    line_start = sum(len(part) for part in lines[:line])
    line_text = lines[line].rstrip("\n")
    return line_start + display_col_to_char_col(line_text, display_col)


def line_col_to_pos(content: str, line: int, col: int) -> int:
    # 根据普通字符列计算一维索引。当前代码主要保留作通用工具，
    # 与 display 版本相比，它不考虑中文等宽字符的屏幕宽度。
    lines = content.splitlines(keepends=True)
    if not lines:
        return 0
    line = max(0, min(line, len(lines) - 1))
    line_start = sum(len(part) for part in lines[:line])
    line_text = lines[line]
    visible_len = len(line_text.rstrip("\n"))
    return line_start + max(0, min(col, visible_len))


def split_lines_for_display(content: str) -> List[str]:
    # str.split("\n") 会在空文档时返回 [""]，正好可以渲染出一行空行。
    lines = content.split("\n")
    return lines if lines else [""]


def is_insertable_character(value: str) -> bool:
    # 只允许单个、非控制字符进入文档；快捷键和终端控制字符在 _handle_key 里处理。
    if len(value) != 1 or value in IGNORED_INPUT:
        return False
    return unicodedata.category(value)[0] != "C"


class TerminalClient:
    """curses 终端客户端。

    网络读取在后台线程中完成，UI 主循环只从 incoming 队列取消息并渲染。
    这样服务端消息到达时不会卡住键盘输入，也不会让 curses 调用跨线程执行。
    """

    def __init__(self, host: str, port: int, client_id: str):
        self.host = host
        self.port = port
        self.client_id = client_id
        self.transport: JsonLineSocket
        self.state: ClientSyncState
        # reader_thread 把 socket 消息放进队列，UI 线程再统一消费。
        self.incoming: "queue.Queue[Message]" = queue.Queue()
        self.reader_thread: threading.Thread
        # cursor_pos 是文档中的字符索引，不是屏幕坐标。
        self.cursor_pos = 0
        # scroll_line 表示当前屏幕正文区域顶部对应文档的哪一行。
        self.scroll_line = 0
        self.running = True

    def connect(self) -> None:
        # TCP_NODELAY 降低按键消息延迟；协作编辑通常每次只发送很小的 JSON。
        sock = socket.create_connection((self.host, self.port))
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.transport = JsonLineSocket(sock)
        self.state = ClientSyncState(self.client_id, self.transport.send_message)
        # JOIN 是协议握手第一步，服务端随后返回 WELCOME 和 DOC_STATE。
        self.transport.send_message({"type": "JOIN", "client_id": self.client_id})
        self.reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self.reader_thread.start()

    def run(self) -> None:
        if curses is None:
            raise RuntimeError("client.py requires curses; please run it in Linux or WSL")
        self.connect()
        # curses.wrapper 会初始化/恢复终端状态，即使异常退出也尽量把终端还原。
        curses.wrapper(self._curses_main)

    def _reader_loop(self) -> None:
        # 后台线程只做阻塞 recv，不直接碰 curses 界面。
        while self.running:
            try:
                self.incoming.put(self.transport.recv_message())
            except Exception as exc:
                self.incoming.put({"type": "ERROR", "message": str(exc)})
                return

    def _curses_main(self, stdscr: "curses._CursesWindow") -> None:
        curses.curs_set(1)
        # nodelay 让 get_wch 没有按键时立即抛 curses.error，
        # 主循环就能持续刷新远端更新，而不是一直阻塞等键盘输入。
        stdscr.nodelay(True)
        stdscr.keypad(True)
        while self.running:
            self._drain_messages()
            self._render(stdscr)
            try:
                key = stdscr.get_wch()
            except curses.error:
                # 没有按键时短暂休眠，避免空转占满 CPU。
                curses.napms(25)
                continue
            self._handle_key(key)

    def _drain_messages(self) -> None:
        # 一次性取空队列，保证渲染前本地状态尽量追上服务端。
        while True:
            try:
                message = self.incoming.get_nowait()
            except queue.Empty:
                return
            old_content = self.state.content
            self.state.handle_message(message)
            if self.state.content != old_content:
                # 远端操作可能改变文档长度，光标要夹在合法范围内。
                self.cursor_pos = max(0, min(self.cursor_pos, len(self.state.content)))

    def _render(self, stdscr: "curses._CursesWindow") -> None:
        stdscr.erase()
        height, width = stdscr.getmaxyx()
        # 顶部状态栏展示客户端 id、服务端版本、待发送队列和当前同步状态。
        status = (
            f"{self.client_id} | v{self.state.version} | "
            f"pending={len(self.state.pending_queue)} | {self.state.status}"
        )
        stdscr.addnstr(0, 0, status, max(0, width - 1), curses.A_REVERSE)

        lines = split_lines_for_display(self.state.content)
        cursor_line, cursor_col = pos_to_line_col(self.state.content, self.cursor_pos)
        # 根据光标所在行调整滚动窗口，保证光标尽量保持在可视区域。
        if cursor_line < self.scroll_line:
            self.scroll_line = cursor_line
        if cursor_line >= self.scroll_line + height - 2:
            self.scroll_line = cursor_line - height + 3

        for row in range(1, height - 1):
            line_index = self.scroll_line + row - 1
            if line_index >= len(lines):
                break
            # 左侧固定显示行号，正文从第 6 列开始。
            prefix = f"{line_index + 1:>4} "
            body_width = max(0, width - len(prefix) - 1)
            stdscr.addnstr(row, 0, prefix, width - 1, curses.A_DIM)
            stdscr.addnstr(row, len(prefix), lines[line_index], body_width)

        help_text = "Ctrl-S save | Ctrl-Q quit"
        stdscr.addnstr(height - 1, 0, help_text, max(0, width - 1), curses.A_REVERSE)
        screen_row = cursor_line - self.scroll_line + 1
        current_line = lines[cursor_line] if cursor_line < len(lines) else ""
        # cursor_col 是字符列，screen_col 需要按实际显示宽度换算。
        screen_col = text_display_width(current_line[:cursor_col]) + 5
        if 1 <= screen_row < height - 1 and 0 <= screen_col < width:
            stdscr.move(screen_row, screen_col)
        stdscr.refresh()

    def _handle_key(self, key: KeyInput) -> None:
        # curses 可能把 Ctrl 键返回为整数，也可能返回控制字符字符串，
        # 所以这里同时兼容两种形式。
        if key in (CTRL_Q, CONTROL_Q):
            self.running = False
            return
        if key in (CTRL_S, CONTROL_S):
            self.transport.send_message({"type": "SAVE"})
            return
        if key in BACKSPACE_KEYS:
            self._delete_before_cursor()
            return
        if isinstance(key, str):
            # 普通字符输入先生成本地 Operation，由 ClientSyncState 排队发送。
            if key == "\n":
                self.state.queue_operation(Operation("insert", self.cursor_pos, "\n"))
                self.cursor_pos += 1
                return
            if is_insertable_character(key):
                self.state.queue_operation(Operation("insert", self.cursor_pos, key))
                self.cursor_pos += 1
            return
        if key == curses.KEY_LEFT:
            # 左右移动只改变本地光标，不产生协作操作。
            self.cursor_pos = max(0, self.cursor_pos - 1)
            return
        if key == curses.KEY_RIGHT:
            self.cursor_pos = min(len(self.state.content), self.cursor_pos + 1)
            return
        if key == curses.KEY_UP:
            line, col = pos_to_line_col(self.state.content, self.cursor_pos)
            current_line = split_lines_for_display(self.state.content)[line]
            # 上下移动保持视觉列，避免光标穿过中文宽字符时左右跳动。
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
            self._delete_before_cursor()
            return
        if key in (curses.KEY_ENTER, 10, 13):
            self.state.queue_operation(Operation("insert", self.cursor_pos, "\n"))
            self.cursor_pos += 1

    def _delete_before_cursor(self) -> None:
        # Backspace 删除光标前一个字符，并把光标移动到被删字符的位置。
        if self.cursor_pos > 0:
            delete_pos = self.cursor_pos - 1
            self.state.queue_operation(Operation("delete", delete_pos))
            self.cursor_pos = delete_pos


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
