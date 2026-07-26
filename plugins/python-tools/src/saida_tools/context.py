"""Public context passed to Python recipe entry points."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from .engine import SaidaTool
from .events import Reporter
from .manifest import ToolSpec
from .transaction import OutputTransaction, safe_relative


class ToolContext:
    def __init__(self, *, project_root: Path, tool: ToolSpec,
                 parameters: dict[str, Any], reporter: Reporter,
                 transaction: OutputTransaction, saida_tool: SaidaTool):
        self.project_root = project_root.resolve()
        self.tool = tool
        self.params = parameters
        self.reporter = reporter
        self.transaction = transaction
        self.saida_tool = saida_tool

    def project_path(self, relative: str | Path) -> Path:
        return self.project_root / safe_relative(relative)

    def inputs(self, pattern: str | None = None) -> list[Path]:
        patterns = (pattern,) if pattern is not None else self.tool.inputs
        found: set[Path] = set()
        for current in patterns:
            found.update(path.resolve() for path in self.project_root.glob(current)
                         if path.is_file())
        return sorted(found, key=lambda path: path.as_posix())

    def output_path(self, relative: str | Path) -> Path:
        return self.transaction.output_path(relative)

    def write_text(self, relative: str | Path, content: str,
                   *, encoding: str = "utf-8") -> Path:
        path = self.output_path(relative)
        path.write_text(content, encoding=encoding)
        return path

    def write_bytes(self, relative: str | Path, content: bytes) -> Path:
        path = self.output_path(relative)
        path.write_bytes(content)
        return path

    def write_json(self, relative: str | Path, value: Any,
                   *, indent: int = 2) -> Path:
        return self.write_text(
            relative,
            json.dumps(value, indent=indent, ensure_ascii=False) + "\n",
        )
