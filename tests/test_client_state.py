import unittest

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


if __name__ == "__main__":
    unittest.main()
