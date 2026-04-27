import unittest

from document import CollaborativeDocument


class CollaborativeDocumentTests(unittest.TestCase):
    def test_delete_position_transforms_after_prior_inserts(self):
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
        self.assertEqual(result.ack["server_version"], 5)
        self.assertEqual(result.ack["op"], {"kind": "delete", "pos": 9})
        self.assertEqual(doc.content(), "hello!!!!world")

    def test_same_character_delete_becomes_noop(self):
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
        self.assertEqual(second.ack["op"]["kind"], "noop")
        self.assertEqual(doc.content(), "ac")
        self.assertEqual(doc.version, 2)

    def test_history_expiry_requests_resync(self):
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
        self.assertEqual(expired.messages[0]["type"], "RESYNC_REQUIRED")
        self.assertEqual(expired.messages[1]["type"], "DOC_STATE")

    def test_server_version_is_strictly_incremented(self):
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

