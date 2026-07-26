"""Execution engine for declared Python and subprocess tools."""

from __future__ import annotations

from dataclasses import dataclass
import importlib
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import time
from typing import Any

from .cache import ToolCache
from .context import ToolContext
from .engine import SaidaTool
from .events import Reporter
from .manifest import Manifest, ToolSpec
from .transaction import OutputTransaction


@dataclass(frozen=True)
class ToolRunResult:
    ok: bool
    cached: bool
    duration_ms: int
    artifacts: tuple[Path, ...] = ()
    error: str = ""


def _entry_source(project_root: Path, entry: str) -> Path | None:
    module_name, _function_name = entry.split(":", 1)
    added = False
    project = str(project_root)
    if project not in sys.path:
        sys.path.insert(0, project)
        added = True
    try:
        spec = importlib.util.find_spec(module_name)
        if spec and spec.origin and spec.origin not in {"built-in", "frozen"}:
            return Path(spec.origin)
        return None
    finally:
        if added:
            sys.path.remove(project)


def _call_entry(project_root: Path, entry: str, context: ToolContext) -> Any:
    module_name, function_name = entry.split(":", 1)
    project = str(project_root)
    added = False
    if project not in sys.path:
        sys.path.insert(0, project)
        added = True
    try:
        module = importlib.import_module(module_name)
        function = getattr(module, function_name, None)
        if not callable(function):
            raise RuntimeError(f"entry '{entry}' is not callable")
        return function(context)
    finally:
        if added:
            sys.path.remove(project)


def _expand_argument(argument: str, project_root: Path, stage_root: Path,
                     parameters: dict[str, Any]) -> str:
    result = argument.replace("${project}", str(project_root))
    result = result.replace("${stage}", str(stage_root))
    for name, value in parameters.items():
        result = result.replace("${param." + name + "}", str(value))
    return result


def _run_command(tool: ToolSpec, context: ToolContext) -> None:
    assert tool.command is not None
    # External tools commonly expect the parent of an exact output path to
    # exist. Preparing it inside staging keeps their command lines simple.
    for output in tool.outputs:
        if not any(character in output for character in "*?["):
            context.output_path(output).parent.mkdir(parents=True, exist_ok=True)
    command = [
        _expand_argument(item, context.project_root, context.transaction.stage,
                         context.params)
        for item in tool.command
    ]
    environment = os.environ.copy()
    environment.update({
        "SAIDA_PROJECT_ROOT": str(context.project_root),
        "SAIDA_STAGE_ROOT": str(context.transaction.stage),
        "SAIDA_TOOL_NAME": tool.name,
    })
    process = subprocess.run(
        command,
        cwd=context.project_root,
        env=environment,
        text=True,
        capture_output=True,
        shell=False,
        check=False,
    )
    if process.stdout:
        for line in process.stdout.splitlines():
            context.reporter.info(line)
    if process.returncode != 0:
        detail = process.stderr.strip() or f"exit code {process.returncode}"
        raise RuntimeError(f"command failed: {detail}")


class ToolRunner:
    def __init__(self, manifest: Manifest, reporter: Reporter,
                 *, saida_tool_override: str | Path | None = None):
        self.manifest = manifest
        self.reporter = reporter
        configured = saida_tool_override or manifest.saida_tool
        self.saida_tool = SaidaTool(
            configured, project_root=manifest.project_root)
        self.cache = ToolCache(manifest.project_root)

    def run(self, name: str, supplied_parameters: dict[str, Any] | None = None,
            *, force: bool = False) -> ToolRunResult:
        started = time.perf_counter()
        diagnostic_start = len(self.reporter.diagnostics)
        tool = self.manifest.tools.get(name)
        if tool is None:
            error = f"unknown tool '{name}'"
            self.reporter.result(False, error)
            return ToolRunResult(False, False, 0, error=error)
        try:
            parameters = tool.resolve_parameters(supplied_parameters or {})
        except Exception as error:
            self.reporter.result(False, str(error))
            return ToolRunResult(False, False, 0, error=str(error))

        extra_inputs: tuple[Path, ...] = ()
        if tool.entry:
            source = _entry_source(self.manifest.project_root, tool.entry)
            if source:
                extra_inputs = (source,)
        key, inputs = self.cache.key(tool, parameters, extra_inputs)
        if tool.cache and not force and self.cache.is_fresh(tool, key):
            duration = int((time.perf_counter() - started) * 1000)
            self.reporter.result(
                True, f"{name}: up to date", cached=True, durationMs=duration)
            return ToolRunResult(True, True, duration)

        transaction = OutputTransaction(
            self.manifest.project_root, tool.outputs)
        context = ToolContext(
            project_root=self.manifest.project_root,
            tool=tool,
            parameters=parameters,
            reporter=self.reporter,
            transaction=transaction,
            saida_tool=self.saida_tool,
        )
        try:
            result = (
                _call_entry(self.manifest.project_root, tool.entry, context)
                if tool.entry else _run_command(tool, context)
            )
            if result is False:
                raise RuntimeError("tool returned failure")
            if any(
                item.severity == "error"
                for item in self.reporter.diagnostics[diagnostic_start:]
            ):
                raise RuntimeError("tool reported one or more errors")
            artifacts = tuple(transaction.commit())
            if tool.cache:
                self.cache.record(tool, key, inputs)
            for artifact in artifacts:
                self.reporter.artifact(artifact)
            duration = int((time.perf_counter() - started) * 1000)
            self.reporter.result(
                True, f"{name}: completed", cached=False, durationMs=duration)
            return ToolRunResult(True, False, duration, artifacts)
        except Exception as error:
            transaction.rollback()
            duration = int((time.perf_counter() - started) * 1000)
            self.reporter.diagnostic("error", str(error), code="tool.failed")
            self.reporter.result(
                False, f"{name}: failed", durationMs=duration)
            return ToolRunResult(
                False, False, duration, error=str(error))
