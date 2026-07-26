import io
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
import json

from saida_tools.events import Reporter
from saida_tools.manifest import load_manifest
from saida_tools.runner import ToolRunner


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: shutil.rmtree(self.root, ignore_errors=True))
        (self.root / "source.txt").write_text("hello", encoding="utf-8")
        (self.root / "recipe_for_test.py").write_text("""
def run(context):
    source = context.project_path("source.txt").read_text(encoding="utf-8")
    context.write_text("generated/result.txt", source + context.params["suffix"])
""", encoding="utf-8")
        (self.root / "saida.tools.toml").write_text("""
schema = 1
[tools.generate]
entry = "recipe_for_test:run"
inputs = ["source.txt"]
outputs = ["generated/result.txt"]
[tools.generate.params.suffix]
type = "string"
default = "!"
""", encoding="utf-8")
        self.addCleanup(lambda: sys.modules.pop("recipe_for_test", None))

    def test_runs_then_uses_cache(self):
        manifest = load_manifest(self.root / "saida.tools.toml")
        reporter = Reporter(stream=io.StringIO())
        runner = ToolRunner(manifest, reporter)
        first = runner.run("generate")
        self.assertTrue(first.ok)
        self.assertFalse(first.cached)
        self.assertEqual(
            (self.root / "generated/result.txt").read_text(encoding="utf-8"),
            "hello!",
        )
        second = runner.run("generate")
        self.assertTrue(second.ok)
        self.assertTrue(second.cached)

    def test_parameter_changes_output_and_cache_key(self):
        manifest = load_manifest(self.root / "saida.tools.toml")
        runner = ToolRunner(manifest, Reporter(stream=io.StringIO()))
        result = runner.run("generate", {"suffix": "?"})
        self.assertTrue(result.ok)
        self.assertEqual(
            (self.root / "generated/result.txt").read_text(encoding="utf-8"),
            "hello?",
        )

    def test_prior_error_does_not_poison_a_later_run(self):
        (self.root / "sequence_recipe.py").write_text("""
def fail(context):
    context.reporter.diagnostic("error", "expected failure")

def recover(context):
    context.write_text("generated/recovered.txt", "recovered")
""", encoding="utf-8")
        (self.root / "saida.tools.toml").write_text("""
schema = 1
[tools.fail]
entry = "sequence_recipe:fail"
outputs = []
cache = false
[tools.recover]
entry = "sequence_recipe:recover"
outputs = ["generated/recovered.txt"]
cache = false
""", encoding="utf-8")
        self.addCleanup(lambda: sys.modules.pop("sequence_recipe", None))
        reporter = Reporter(stream=io.StringIO())
        runner = ToolRunner(
            load_manifest(self.root / "saida.tools.toml"), reporter)

        self.assertFalse(runner.run("fail").ok)
        self.assertTrue(runner.run("recover").ok)
        self.assertEqual(
            (self.root / "generated/recovered.txt").read_text(
                encoding="utf-8"),
            "recovered",
        )

    def test_external_command_writes_to_prepared_stage_directory(self):
        python = json.dumps(sys.executable)
        code = json.dumps(
            "from pathlib import Path; import sys; "
            "Path(sys.argv[1]).write_text('external', encoding='utf-8')")
        (self.root / "saida.tools.toml").write_text(f"""
schema = 1
[tools.external]
command = [{python}, "-c", {code}, "${{stage}}/nested/output.txt"]
outputs = ["nested/output.txt"]
cache = false
""", encoding="utf-8")
        runner = ToolRunner(
            load_manifest(self.root / "saida.tools.toml"),
            Reporter(stream=io.StringIO()),
        )

        self.assertTrue(runner.run("external").ok)
        self.assertEqual(
            (self.root / "nested/output.txt").read_text(encoding="utf-8"),
            "external",
        )


if __name__ == "__main__":
    unittest.main()
