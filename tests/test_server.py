import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from server import CollaborativeServer, format_operation_event, format_save_event, format_server_event


class ServerOutputTests(unittest.TestCase):
    def test_format_insert_operation_event(self):
        message = format_operation_event(
            {
                "event": "operation",
                "server_version": 12,
                "client_id": "u1",
                "base_version": 11,
                "transformed_op": {"kind": "insert", "pos": 5, "char": "你"},
            }
        )

        self.assertEqual(message, "OP v12 client=u1 insert pos=5 char='你' base=11")

    def test_format_delete_operation_event(self):
        message = format_operation_event(
            {
                "event": "operation",
                "server_version": 13,
                "client_id": "u2",
                "base_version": 12,
                "transformed_op": {"kind": "delete", "pos": 3},
            }
        )

        self.assertEqual(message, "OP v13 client=u2 delete pos=3 base=12")

    def test_format_save_event(self):
        message = format_save_event("requested", "u1", 13, Path("shared.py"))

        self.assertEqual(message, "SAVE requested client=u1 version=13 file=shared.py")

    def test_format_join_leave_events(self):
        self.assertEqual(
            format_server_event({"event": "join", "client_id": "u1", "address": "127.0.0.1"}),
            "JOIN client=u1 address=127.0.0.1",
        )
        self.assertEqual(
            format_server_event({"event": "leave", "client_id": "u1"}),
            "LEAVE client=u1",
        )

    def test_shutdown_ignores_non_owner_process(self):
        with (
            patch("server.multiprocessing.Queue", return_value=Mock()),
            patch("server.multiprocessing.Process", return_value=Mock()),
        ):
            server = CollaborativeServer("127.0.0.1", 0, Path("shared.py"))

        with patch("server.os.getpid", return_value=server._owner_pid + 1):
            server.shutdown()

        self.assertFalse(server._stop_event.is_set())


if __name__ == "__main__":
    unittest.main()
