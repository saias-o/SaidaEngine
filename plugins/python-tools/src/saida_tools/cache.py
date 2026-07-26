"""Content-addressed receipts for optional project tools."""

from __future__ import annotations

from dataclasses import asdict
import hashlib
import json
from pathlib import Path
import re
from typing import Any

from .manifest import ToolSpec


def _files_for_patterns(root: Path, patterns: tuple[str, ...]) -> list[Path]:
    found: set[Path] = set()
    for pattern in patterns:
        for path in root.glob(pattern):
            if path.is_file():
                found.add(path.resolve())
    return sorted(found, key=lambda path: path.as_posix())


def _outputs_exist(root: Path, patterns: tuple[str, ...]) -> bool:
    for pattern in patterns:
        matches = list(root.glob(pattern))
        if not matches:
            return False
    return True


class ToolCache:
    def __init__(self, project_root: Path):
        self.project_root = project_root.resolve()
        self.root = self.project_root / ".saida" / "python-tools" / "cache"

    def _receipt_path(self, tool_name: str) -> Path:
        safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", tool_name)
        return self.root / f"{safe}.json"

    def key(self, tool: ToolSpec, parameters: dict[str, Any],
            extra_inputs: tuple[Path, ...] = ()) -> tuple[str, list[Path]]:
        files = _files_for_patterns(self.project_root, tool.inputs)
        files = sorted(set(files).union(path.resolve() for path in extra_inputs))
        digest = hashlib.sha256()
        contract = {
            "tool": asdict(tool),
            "parameters": parameters,
        }
        digest.update(json.dumps(
            contract, sort_keys=True, separators=(",", ":"), default=str,
        ).encode("utf-8"))
        for path in files:
            try:
                relative = path.relative_to(self.project_root).as_posix()
            except ValueError:
                relative = str(path)
            digest.update(relative.encode("utf-8"))
            digest.update(b"\0")
            with path.open("rb") as file:
                while chunk := file.read(1024 * 1024):
                    digest.update(chunk)
        return digest.hexdigest(), files

    def is_fresh(self, tool: ToolSpec, key: str) -> bool:
        if not tool.outputs or not _outputs_exist(self.project_root, tool.outputs):
            return False
        path = self._receipt_path(tool.name)
        try:
            receipt = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return False
        return receipt.get("schema") == 1 and receipt.get("key") == key

    def record(self, tool: ToolSpec, key: str, inputs: list[Path]) -> None:
        self.root.mkdir(parents=True, exist_ok=True)
        path = self._receipt_path(tool.name)
        temporary = path.with_suffix(".tmp")
        receipt = {
            "schema": 1,
            "tool": tool.name,
            "key": key,
            "inputs": [
                path.relative_to(self.project_root).as_posix()
                if path.is_relative_to(self.project_root) else str(path)
                for path in inputs
            ],
            "outputs": list(tool.outputs),
        }
        temporary.write_text(
            json.dumps(receipt, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        temporary.replace(path)
