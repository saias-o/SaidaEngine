"""Optional subprocess adapter for the native ``saida_tool`` contract."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
from typing import Any


class SaidaToolError(RuntimeError):
    pass


def _executable_name() -> str:
    return "saida_tool.exe" if os.name == "nt" else "saida_tool"


class SaidaTool:
    def __init__(self, executable: str | Path | None = None,
                 *, project_root: str | Path | None = None):
        self.project_root = (
            Path(project_root).resolve() if project_root is not None else None)
        self.executable = self._locate(executable)

    def _locate(self, explicit: str | Path | None) -> Path | None:
        candidates: list[Path] = []
        configured = explicit or os.environ.get("SAIDA_TOOL")
        if configured:
            candidate = Path(configured)
            if not candidate.is_absolute() and self.project_root is not None:
                candidate = self.project_root / candidate
            candidates.append(candidate)
        found = shutil.which(_executable_name())
        if found:
            candidates.append(Path(found))

        # Works when this optional plugin remains in the engine repository, but
        # does not make that layout mandatory for an installed wheel.
        engine_root = Path(__file__).resolve().parents[4]
        candidates.append(engine_root / "build" / "bin" / _executable_name())
        if self.project_root is not None:
            candidates.append(
                self.project_root.parent / "saidaengine" / "build" / "bin"
                / _executable_name())
        for candidate in candidates:
            if candidate.is_file():
                return candidate.resolve()
        return None

    @property
    def available(self) -> bool:
        return self.executable is not None

    def require(self) -> Path:
        if self.executable is None:
            raise SaidaToolError(
                "saida_tool was not found; set SAIDA_TOOL or engine.saida_tool")
        return self.executable

    def run(self, *arguments: str, stdin: str | None = None,
            expect_json: bool = False) -> Any:
        executable = self.require()
        process = subprocess.run(
            [str(executable), *map(str, arguments)],
            input=stdin,
            text=True,
            capture_output=True,
            cwd=str(self.project_root) if self.project_root else None,
            check=False,
        )
        parsed = None
        if expect_json:
            try:
                parsed = json.loads(process.stdout)
            except json.JSONDecodeError as error:
                if process.returncode != 0:
                    detail = process.stderr.strip() or process.stdout.strip()
                    raise SaidaToolError(
                        f"saida_tool exited with {process.returncode}: {detail}"
                    ) from error
                raise SaidaToolError(
                    "saida_tool did not return valid JSON") from error
        # Exit 1 is the native validation contract for a well-formed JSON
        # report whose inspected input is invalid. Invocation and I/O failures
        # use exit 2 and remain exceptions.
        if process.returncode == 1 and isinstance(parsed, dict):
            return parsed
        if process.returncode != 0:
            detail = process.stderr.strip() or process.stdout.strip()
            raise SaidaToolError(
                f"saida_tool exited with {process.returncode}: {detail}")
        if expect_json:
            return parsed
        return process.stdout

    def describe_engine(self) -> dict[str, Any]:
        return self.run("describe-engine", expect_json=True)

    def validate_script(self, path: str | Path) -> dict[str, Any]:
        return self.run("validate-script", str(path), expect_json=True)

    def validate_authoring_snapshot(self, path: str | Path) -> dict[str, Any]:
        """Validate a SceneSnapshot, not a runtime ``.scene`` document."""
        return self.run("validate-scene", str(path), expect_json=True)

    def validate_ops(self, path: str | Path) -> dict[str, Any]:
        return self.run("validate-ops", str(path), expect_json=True)

    def validate_scenario(self, path: str | Path) -> dict[str, Any]:
        return self.run("validate-scenario", str(path), expect_json=True)

    def inspect_animation(self, path: str | Path) -> dict[str, Any]:
        return self.run("inspect-anim", str(path), expect_json=True)
