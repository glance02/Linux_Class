import unittest

from client import BACKSPACE_KEYS, TerminalClient, display_col_to_char_col, is_insertable_character, text_display_width
from client_state import ClientSyncState
from document import Operation


class ClientSyncStateTests(unittest.TestCase):
    def test_pending_queue_sends_next_only_after_ack(self):
        sent = []
        state = ClientSyncState("u1", sent.append)
        state.set_document("abc", 0)

        first_id = state.queue_operation(Operation("insert", 3, "x"))
        second_id = state.queue_operation(Operation("insert", 4, "y"))

        self.assertEqual(len(sent), 1)
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
        self.assertIsNone(state.inflight_op)
        self.assertEqual(len(state.pending_queue), 0)

    def test_remote_gap_requests_document(self):
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
        sent = []
        state = ClientSyncState("u1", sent.append)
        state.handle_message({"type": "WELCOME", "client_id": "u2"})
        state.set_document("", 0)
        op_id = state.queue_operation(Operation("insert", 0, "x"))

        self.assertEqual(state.client_id, "u2")
        self.assertEqual(op_id, "u2-1")
        self.assertEqual(sent[0]["client_id"], "u2")

    def test_chinese_character_is_insertable(self):
        self.assertTrue(is_insertable_character("你"))
        self.assertFalse(is_insertable_character("\x13"))
        self.assertFalse(is_insertable_character("\t"))

    def test_chinese_display_width_helpers(self):
        self.assertEqual(text_display_width("a你b"), 4)
        self.assertEqual(display_col_to_char_col("a你b", 2), 1)
        self.assertEqual(display_col_to_char_col("a你b", 3), 2)

    def test_wide_character_backspace_queues_delete(self):
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
