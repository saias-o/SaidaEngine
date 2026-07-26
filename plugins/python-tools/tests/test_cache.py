import tempfile
from pathlib import Path
import shutil
import unittest

from saida_tools.cache import ToolCache
from saida_tools.manifest import ToolSpec


class CacheTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: shutil.rmtree(self.root, ignore_errors=True))
        (self.root / "data").mkdir()
        (self.root / "data/source.txt").write_text("one", encoding="utf-8")
        self.tool = ToolSpec(
            name="build",
            entry="recipe:run",
            inputs=("data/*.txt",),
            outputs=("generated/out.txt",),
        )

    def test_content_change_invalidates_key(self):
        cache = ToolCache(self.root)
        first, _ = cache.key(self.tool, {})
        (self.root / "data/source.txt").write_text("two", encoding="utf-8")
        second, _ = cache.key(self.tool, {})
        self.assertNotEqual(first, second)

    def test_receipt_requires_outputs(self):
        cache = ToolCache(self.root)
        key, inputs = cache.key(self.tool, {})
        cache.record(self.tool, key, inputs)
        self.assertFalse(cache.is_fresh(self.tool, key))
        output = self.root / "generated/out.txt"
        output.parent.mkdir()
        output.write_text("done", encoding="utf-8")
        self.assertTrue(cache.is_fresh(self.tool, key))


if __name__ == "__main__":
    unittest.main()
