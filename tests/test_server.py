import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from server import CollaborativeServer, format_operation_event, format_save_event, format_server_event


class ServerOutputTests(unittest.TestCase):
    """验证服务端终端输出和关闭保护逻辑。

    这些测试不启动真实 socket，而是只检查纯格式化函数和 shutdown 的边界行为。
    """

    def test_format_insert_operation_event(self):
        # insert 日志需要额外展示 char，便于观察中文字符等输入是否正确同步。
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
        # delete 日志没有 char 字段，只展示操作类型、位置和基准版本。
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
        # 客户端触发的保存要在终端输出中标明 client_id。
        message = format_save_event("requested", "u1", 13, Path("shared.py"))

        self.assertEqual(message, "SAVE requested client=u1 version=13 file=shared.py")

    def test_format_join_leave_events(self):
        # 加入和退出事件用于课堂演示时观察客户端连接生命周期。
        self.assertEqual(
            format_server_event({"event": "join", "client_id": "u1", "address": "127.0.0.1"}),
            "JOIN client=u1 address=127.0.0.1",
        )
        self.assertEqual(
            format_server_event({"event": "leave", "client_id": "u1"}),
            "LEAVE client=u1",
        )

    def test_shutdown_ignores_non_owner_process(self):
        # Windows multiprocessing 可能让子进程持有 server 对象副本。
        # 非 owner 进程调用 shutdown 时必须直接返回，不能关闭父进程资源。
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
