import tempfile
from pathlib import Path
import shutil
import unittest

from saida_tools.transaction import OutputTransaction, TransactionError


class TransactionTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: shutil.rmtree(self.root, ignore_errors=True))

    def test_commits_declared_file(self):
        transaction = OutputTransaction(self.root, ("generated/result.txt",))
        staged = transaction.output_path("generated/result.txt")
        staged.write_text("new", encoding="utf-8")
        artifacts = transaction.commit()
        self.assertEqual(
            (self.root / "generated/result.txt").read_text(encoding="utf-8"),
            "new",
        )
        self.assertEqual(artifacts, [self.root / "generated/result.txt"])

    def test_replaces_existing_file(self):
        destination = self.root / "generated/result.txt"
        destination.parent.mkdir()
        destination.write_text("old", encoding="utf-8")
        transaction = OutputTransaction(self.root, ("generated/result.txt",))
        transaction.output_path("generated/result.txt").write_text(
            "new", encoding="utf-8")
        transaction.commit()
        self.assertEqual(destination.read_text(encoding="utf-8"), "new")

    def test_rejects_undeclared_output(self):
        transaction = OutputTransaction(self.root, ("generated/result.txt",))
        self.addCleanup(transaction.rollback)
        with self.assertRaises(TransactionError):
            transaction.output_path("other.txt")

    def test_rejects_parent_escape(self):
        transaction = OutputTransaction(self.root, ("generated/",))
        self.addCleanup(transaction.rollback)
        with self.assertRaises(TransactionError):
            transaction.output_path("../outside.txt")

    def test_rejects_windows_drive_escape(self):
        transaction = OutputTransaction(self.root, ("**/*",))
        self.addCleanup(transaction.rollback)
        with self.assertRaises(TransactionError):
            transaction.output_path(r"C:\outside.txt")


if __name__ == "__main__":
    unittest.main()
