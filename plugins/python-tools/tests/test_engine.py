from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

from saida_tools.engine import SaidaTool, SaidaToolError


class SaidaToolTests(unittest.TestCase):
    def setUp(self):
        self.adapter = SaidaTool()
        self.adapter.executable = Path("saida_tool")

    @staticmethod
    def _completed(code, stdout="", stderr=""):
        return subprocess.CompletedProcess(
            ["saida_tool"], code, stdout=stdout, stderr=stderr)

    def test_validation_exit_one_returns_json_report(self):
        process = self._completed(1, '{"ok":false,"issues":[]}\n')
        with patch("saida_tools.engine.subprocess.run", return_value=process):
            report = self.adapter.validate_authoring_snapshot("snapshot.json")
        self.assertFalse(report["ok"])

    def test_usage_failure_remains_an_exception(self):
        process = self._completed(2, stderr="bad invocation")
        with patch("saida_tools.engine.subprocess.run", return_value=process):
            with self.assertRaises(SaidaToolError):
                self.adapter.validate_authoring_snapshot("snapshot.json")

    def test_successful_json_contract(self):
        process = self._completed(0, '{"ok":true}\n')
        with patch("saida_tools.engine.subprocess.run", return_value=process):
            self.assertTrue(self.adapter.validate_ops("ops.json")["ok"])


if __name__ == "__main__":
    unittest.main()
