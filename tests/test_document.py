import unittest

from document import CollaborativeDocument


class CollaborativeDocumentTests(unittest.TestCase):
    """验证服务端权威文档模型的 OT、版本和历史裁剪行为。"""

    def test_delete_position_transforms_after_prior_inserts(self):
        # 场景：A 已经在旧位置附近连续插入，B 仍基于版本 0 删除原来的空格。
        # 服务端必须把 B 的删除位置向右平移，才能删除“原本想删的那个字符”。
        doc = CollaborativeDocument("hello world")
        for index, pos in enumerate([5, 6, 7, 8], start=1):
            result = doc.process_operation(
                client_id="A",
                op_id=f"A-{index}",
                base_version=index - 1,
                op_payload={"kind": "insert", "pos": pos, "char": "!"},
            )
            self.assertEqual(result.status, "ok")

        result = doc.process_operation(
            client_id="B",
            op_id="B-1",
            base_version=0,
            op_payload={"kind": "delete", "pos": 5},
        )

        self.assertEqual(result.status, "ok")
        # 四次插入之后，B 的删除成为第 5 个服务端权威操作。
        self.assertEqual(result.ack["server_version"], 5)
        # 原始 pos=5 经过四次 insert 变换后落在 pos=9。
        self.assertEqual(result.ack["op"], {"kind": "delete", "pos": 9})
        self.assertEqual(doc.content(), "hello!!!!world")

    def test_same_character_delete_becomes_noop(self):
        # 两个客户端都基于版本 0 删除同一个字符。第一个删除生效后，
        # 第二个删除不能再删一次，否则会误删后面的字符，所以应变成 noop。
        doc = CollaborativeDocument("abc")
        first = doc.process_operation(
            client_id="A",
            op_id="A-1",
            base_version=0,
            op_payload={"kind": "delete", "pos": 1},
        )
        second = doc.process_operation(
            client_id="B",
            op_id="B-1",
            base_version=0,
            op_payload={"kind": "delete", "pos": 1},
        )

        self.assertEqual(first.status, "ok")
        self.assertEqual(second.status, "ok")
        # noop 仍然会获得 ACK 和版本号，保证所有客户端版本推进一致。
        self.assertEqual(second.ack["op"]["kind"], "noop")
        self.assertEqual(doc.content(), "ac")
        self.assertEqual(doc.version, 2)

    def test_history_expiry_requests_resync(self):
        # history_limit=2 表示服务端只保留最近两条操作。
        # 当客户端基于更旧版本提交操作时，服务端已经无法安全做 OT。
        doc = CollaborativeDocument("", history_limit=2)
        for version in range(3):
            result = doc.process_operation(
                client_id="A",
                op_id=f"A-{version}",
                base_version=version,
                op_payload={"kind": "insert", "pos": version, "char": "x"},
            )
            self.assertEqual(result.status, "ok")

        self.assertEqual(doc.history_start_version, 1)
        expired = doc.process_operation(
            client_id="B",
            op_id="B-1",
            base_version=0,
            op_payload={"kind": "insert", "pos": 0, "char": "y"},
        )

        self.assertEqual(expired.status, "resync")
        # resync 响应同时包含原因消息和完整文档快照，客户端可立即恢复状态。
        self.assertEqual(expired.messages[0]["type"], "RESYNC_REQUIRED")
        self.assertEqual(expired.messages[1]["type"], "DOC_STATE")

    def test_server_version_is_strictly_incremented(self):
        # 每个被接受的操作都必须拿到严格递增的 server_version，
        # 这是所有客户端判断全局顺序的依据。
        doc = CollaborativeDocument("")
        versions = []
        for index, char in enumerate("abc"):
            result = doc.process_operation(
                client_id="A",
                op_id=f"A-{index}",
                base_version=index,
                op_payload={"kind": "insert", "pos": index, "char": char},
            )
            versions.append(result.ack["server_version"])

        self.assertEqual(versions, [1, 2, 3])
        self.assertEqual(doc.content(), "abc")

    def test_insert_chinese_character(self):
        # Python 字符串按 Unicode 字符计数，中文“你”应被视为单个插入字符。
        doc = CollaborativeDocument("")
        result = doc.process_operation(
            client_id="A",
            op_id="A-1",
            base_version=0,
            op_payload={"kind": "insert", "pos": 0, "char": "你"},
        )

        self.assertEqual(result.status, "ok")
        self.assertEqual(result.ack["server_version"], 1)
        self.assertEqual(doc.content(), "你")


if __name__ == "__main__":
    unittest.main()

