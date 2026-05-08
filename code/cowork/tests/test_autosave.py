import unittest
from pathlib import Path
from unittest.mock import patch

from autosave import write_with_backup


class AutosaveTests(unittest.TestCase):
    """验证保存模块中与文件备份相关的可观察行为。"""

    def test_manual_save_writes_backup(self):
        # 手动保存覆盖正式文件前，应先把原文件复制成 .bak。
        # 这里用 mock 隔离真实文件系统，只验证调用顺序和目标路径。
        path = Path("shared.py")

        with (
            patch.object(Path, "exists", return_value=True),
            patch("autosave.shutil.copy2") as copy2,
            patch("autosave.atomic_write_text") as atomic_write,
        ):
            write_with_backup(path, "new")

        copy2.assert_called_once_with(path, path.with_name("shared.py.bak"))
        atomic_write.assert_called_once_with(path, "new")


if __name__ == "__main__":
    unittest.main()
