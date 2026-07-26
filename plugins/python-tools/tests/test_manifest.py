import tempfile
from pathlib import Path
import unittest

from saida_tools.manifest import ManifestError, load_manifest


class ManifestTests(unittest.TestCase):
    def write(self, text: str) -> Path:
        directory = Path(tempfile.mkdtemp())
        path = directory / "saida.tools.toml"
        path.write_text(text, encoding="utf-8")
        self.addCleanup(
            lambda: __import__("shutil").rmtree(directory, ignore_errors=True))
        return path

    def test_loads_typed_tool(self):
        manifest = load_manifest(self.write("""
schema = 1
[tools.generate]
entry = "tools.generate:run"
inputs = ["data/**/*.json"]
outputs = ["scenes/main.scene"]
[tools.generate.params.debug]
type = "boolean"
default = false
"""))
        tool = manifest.tools["generate"]
        self.assertEqual(tool.resolve_parameters({"debug": "yes"}), {"debug": True})
        self.assertEqual(tool.resolve_parameters({}), {"debug": False})

    def test_requires_exact_schema(self):
        with self.assertRaisesRegex(ManifestError, "schema must be exactly 1"):
            load_manifest(self.write("schema = 2\n[tools]\n"))

    def test_rejects_ambiguous_execution(self):
        with self.assertRaisesRegex(ManifestError, "exactly one"):
            load_manifest(self.write("""
schema = 1
[tools.bad]
entry = "bad:run"
command = ["python", "bad.py"]
"""))

    def test_rejects_unknown_fields(self):
        with self.assertRaisesRegex(ManifestError, "unknown field"):
            load_manifest(self.write("""
schema = 1
[tools.bad]
entry = "bad:run"
silentFallback = true
"""))

    def test_rejects_patterns_outside_the_project(self):
        with self.assertRaisesRegex(ManifestError, "project-relative"):
            load_manifest(self.write("""
schema = 1
[tools.bad]
entry = "recipe:run"
inputs = ["../secret.txt"]
outputs = []
"""))

    def test_rejects_windows_absolute_pattern(self):
        with self.assertRaisesRegex(ManifestError, "project-relative"):
            load_manifest(self.write("""
schema = 1
[tools.bad]
entry = "recipe:run"
inputs = ['C:\\outside\\secret.txt']
outputs = []
"""))


if __name__ == "__main__":
    unittest.main()
