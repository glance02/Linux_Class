import unittest

from client import BACKSPACE_KEYS, TerminalClient, display_col_to_char_col, is_insertable_character, text_display_width
from client_state import ClientSyncState
from document import Operation


class ClientSyncStateTests(unittest.TestCase):
    """验证客户端状态机的发送节奏、重同步和宽字符辅助逻辑。"""

    def test_pending_queue_sends_next_only_after_ack(self):
        # 客户端连续输入两个字符时，第二个操作先留在 pending_queue。
        # 只有第一个操作收到 ACK 后，客户端才用新版本发送第二个操作。
        sent = []
        state = ClientSyncState("u1", sent.append)
        state.set_document("abc", 0)

        first_id = state.queue_operation(Operation("insert", 3, "x"))
        second_id = state.queue_operation(Operation("insert", 4, "y"))

        self.assertEqual(len(sent), 1)
        # 第一个操作基于初始版本 0 发出；第二个还没有进入网络层。
        self.assertEqual(sent[0]["op_id"], first_id)
        self.assertEqual(sent[0]["base_version"], 0)
        self.assertEqual(state.inflight_op.op_id, first_id)
        self.assertEqual(len(state.pending_queue), 1)

        state.handle_message(
            {
                "type": "ACK",
                "op_id": first_id,
                "version": 1,
                "op": {"kind": "insert", "pos": 3, "char": "x"},
            }
        )

        # ACK 中的权威操作先应用到本地文本，然后 _pump 继续发送队列头。
        self.assertEqual(state.content, "abcx")
        self.assertEqual(len(sent), 2)
        self.assertEqual(sent[1]["op_id"], second_id)
        self.assertEqual(sent[1]["base_version"], 1)

        state.handle_message(
            {
                "type": "ACK",
                "op_id": second_id,
                "version": 2,
                "op": {"kind": "insert", "pos": 4, "char": "y"},
            }
        )

        self.assertEqual(state.content, "abcxy")
        # 两个 ACK 都处理完后，客户端没有 inflight，也没有待发送操作。
        self.assertIsNone(state.inflight_op)
        self.assertEqual(len(state.pending_queue), 0)

    def test_remote_gap_requests_document(self):
        # 如果收到的远端版本不是当前版本 + 1，说明中间漏了操作。
        # 客户端此时不能直接应用，只能请求完整文档重同步。
        sent = []
        state = ClientSyncState("u1", sent.append)
        state.set_document("", 0)
        state.handle_message(
            {
                "type": "REMOTE_OP",
                "version": 3,
                "op": {"kind": "insert", "pos": 0, "char": "z"},
            }
        )

        self.assertEqual(sent[-1], {"type": "REQUEST_DOC"})

    def test_welcome_updates_server_assigned_client_id(self):
        # 服务端可能因为 id 冲突给客户端分配新 id。
        # 客户端后续生成 op_id 和发送 OP 时必须使用服务端确认后的 id。
        sent = []
        state = ClientSyncState("u1", sent.append)
        state.handle_message({"type": "WELCOME", "client_id": "u2"})
        state.set_document("", 0)
        op_id = state.queue_operation(Operation("insert", 0, "x"))

        self.assertEqual(state.client_id, "u2")
        self.assertEqual(op_id, "u2-1")
        self.assertEqual(sent[0]["client_id"], "u2")

    def test_chinese_character_is_insertable(self):
        # 普通中文字符可以插入；Ctrl-S 和 Tab 这类控制字符不能进入文档。
        self.assertTrue(is_insertable_character("你"))
        self.assertFalse(is_insertable_character("\x13"))
        self.assertFalse(is_insertable_character("\t"))

    def test_chinese_display_width_helpers(self):
        # 终端里中文通常占两个显示列，光标上下移动要按显示宽度计算。
        self.assertEqual(text_display_width("a你b"), 4)
        self.assertEqual(display_col_to_char_col("a你b", 2), 1)
        self.assertEqual(display_col_to_char_col("a你b", 3), 2)

    def test_wide_character_backspace_queues_delete(self):
        # Backspace 不直接修改本地文本，而是排队一个 delete 操作交给服务端确认。
        sent = []
        client = TerminalClient("127.0.0.1", 8765, "u1")
        client.state = ClientSyncState("u1", sent.append)
        client.state.set_document("ab", 0)
        client.cursor_pos = 2

        client._handle_key("\x7f")

        self.assertIn("\x7f", BACKSPACE_KEYS)
        self.assertEqual(sent[0]["op"], {"kind": "delete", "pos": 1})
        self.assertEqual(client.cursor_pos, 1)


if __name__ == "__main__":
    unittest.main()
