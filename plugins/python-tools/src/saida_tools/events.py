"""Human and JSONL event reporting shared by tools and the CLI."""

from __future__ import annotations

from dataclasses import asdict, dataclass
import json
from pathlib import Path
import sys
from typing import TextIO


@dataclass(frozen=True)
class Diagnostic:
    severity: str
    message: str
    file: str | None = None
    line: int | None = None
    code: str | None = None


class Reporter:
    def __init__(self, *, json_lines: bool = False, stream: TextIO | None = None):
        self.json_lines = json_lines
        self.stream = stream or sys.stdout
        self.diagnostics: list[Diagnostic] = []

    def _emit(self, kind: str, **payload: object) -> None:
        event = {"type": kind, **payload}
        if self.json_lines:
            print(json.dumps(event, ensure_ascii=False, separators=(",", ":")),
                  file=self.stream, flush=True)
            return
        if kind == "progress":
            total = payload.get("total")
            current = payload.get("current")
            prefix = f"[{current}/{total}] " if total is not None else ""
            print(prefix + str(payload.get("message", "")), file=self.stream)
        elif kind == "diagnostic":
            location = payload.get("file") or ""
            if payload.get("line") is not None:
                location += f":{payload['line']}"
            if location:
                location += ": "
            print(f"{str(payload.get('severity', 'info')).upper()}: "
                  f"{location}{payload.get('message', '')}", file=self.stream)
        elif kind == "artifact":
            print(f"artifact: {payload.get('path', '')}", file=self.stream)
        elif kind == "result":
            status = "OK" if payload.get("ok") else "FAILED"
            suffix = f" ({payload.get('message')})" if payload.get("message") else ""
            print(f"{status}{suffix}", file=self.stream)
        else:
            print(str(payload.get("message", "")), file=self.stream)

    def info(self, message: str) -> None:
        self._emit("info", message=message)

    def progress(self, current: int, total: int | None, message: str) -> None:
        self._emit("progress", current=current, total=total, message=message)

    def diagnostic(self, severity: str, message: str, *,
                   file: str | Path | None = None, line: int | None = None,
                   code: str | None = None) -> None:
        diagnostic = Diagnostic(
            severity=severity,
            message=message,
            file=str(file) if file is not None else None,
            line=line,
            code=code,
        )
        self.diagnostics.append(diagnostic)
        self._emit("diagnostic", **{
            key: value for key, value in asdict(diagnostic).items()
            if value is not None
        })

    def artifact(self, path: str | Path) -> None:
        self._emit("artifact", path=str(path))

    def result(self, ok: bool, message: str = "", **details: object) -> None:
        self._emit("result", ok=ok, message=message, **details)
