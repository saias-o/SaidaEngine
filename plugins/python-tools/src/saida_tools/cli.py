"""Command-line interface for the optional standalone plugin."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import platform
import sys
from typing import Any

from . import __version__
from .builtins import BUILTIN_TOOLS
from .engine import SaidaTool
from .events import Reporter
from .manifest import ManifestError, find_manifest, load_manifest
from .runner import ToolRunner


DEFAULT_MANIFEST = """\
schema = 1

# The native adapter is optional. Set this only when saida_tool is not on PATH.
# [engine]
# saida_tool = "../saidaengine/build/bin/saida_tool.exe"

[tools.project-audit]
description = "Audit the project, runtime scenes, registry and scripts"
entry = "saida_tools.builtins.project_audit:run"
inputs = ["*.saidaproj", "asset_registry.json", "scenes/**/*.scene", "scripts/**/*.mjs"]
outputs = []
cache = false

[tools.asset-inventory]
description = "Write a deterministic inventory of project assets"
entry = "saida_tools.builtins.asset_inventory:run"
inputs = ["assets/**/*"]
outputs = ["reports/asset-inventory.json"]
cache = true

[tools.asset-inventory.params.output]
type = "path"
default = "reports/asset-inventory.json"
description = "Project-relative report path (must be declared as an output)"
"""


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="saida-python-tools",
        description="Optional standalone Python tooling for SaidaEngine projects",
    )
    parser.add_argument("--version", action="version", version=__version__)
    parser.add_argument(
        "--project", help="project directory or .saidaproj path")
    parser.add_argument(
        "--manifest", help="explicit saida.tools.toml path")
    parser.add_argument(
        "--saida-tool", help="explicit native saida_tool executable")
    parser.add_argument(
        "--json", action="store_true", help="machine-readable output/JSONL events")

    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("validate", help="validate the optional tool manifest")
    commands.add_parser("list", help="list project tools")
    commands.add_parser("builtins", help="list built-in recipe entry points")
    commands.add_parser("doctor", help="inspect the optional plugin environment")

    run = commands.add_parser("run", help="run one declared project tool")
    run.add_argument("tool")
    run.add_argument(
        "--param", action="append", default=[], metavar="NAME=VALUE")
    run.add_argument("--force", action="store_true", help="ignore a fresh cache receipt")

    init = commands.add_parser(
        "init", help="create a standalone saida.tools.toml in a project")
    init.add_argument(
        "--force", action="store_true", help="replace an existing manifest")
    return parser


def _project_root(value: str | None) -> Path:
    path = Path(value).resolve() if value else Path.cwd().resolve()
    if path.is_file():
        return path.parent
    return path


def _manifest_path(args: argparse.Namespace) -> Path:
    if args.manifest:
        return Path(args.manifest).resolve()
    root = _project_root(args.project)
    direct = root / "saida.tools.toml"
    if direct.is_file():
        return direct
    found = find_manifest(root)
    if found:
        return found
    raise ManifestError(
        f"no saida.tools.toml found from {root}; run 'init' to opt in")


def _parameters(values: list[str]) -> dict[str, str]:
    parsed = {}
    for value in values:
        if "=" not in value:
            raise ManifestError(f"--param expects NAME=VALUE, got '{value}'")
        name, raw = value.split("=", 1)
        if not name or name in parsed:
            raise ManifestError(f"invalid or duplicate parameter '{name}'")
        parsed[name] = raw
    return parsed


def _emit(value: Any, json_output: bool) -> None:
    if json_output:
        print(json.dumps(value, ensure_ascii=False, separators=(",", ":")))
    elif isinstance(value, str):
        print(value)
    else:
        print(json.dumps(value, ensure_ascii=False, indent=2))


def _doctor(args: argparse.Namespace) -> int:
    root = _project_root(args.project)
    manifest_path = None
    manifest_error = None
    try:
        manifest_path = _manifest_path(args)
        manifest = load_manifest(manifest_path)
        configured = args.saida_tool or manifest.saida_tool
    except ManifestError as error:
        manifest_error = str(error)
        configured = args.saida_tool
    native = SaidaTool(configured, project_root=root)
    project_files = list(root.glob("*.saidaproj"))
    report = {
        "ok": sys.version_info >= (3, 11),
        "pluginVersion": __version__,
        "python": platform.python_version(),
        "pythonExecutable": sys.executable,
        "projectRoot": str(root),
        "projectFiles": len(project_files),
        "manifest": str(manifest_path) if manifest_path else None,
        "manifestError": manifest_error,
        "saidaTool": str(native.executable) if native.available else None,
        "saidaToolAvailable": native.available,
        "runtimeDependency": False,
        "engineBuildDependency": False,
    }
    _emit(report, args.json)
    return 0 if report["ok"] else 1


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "init":
            root = _project_root(args.project)
            if not root.is_dir():
                raise ManifestError(f"project directory does not exist: {root}")
            destination = root / "saida.tools.toml"
            if destination.exists() and not args.force:
                raise ManifestError(
                    f"{destination} already exists (use --force to replace it)")
            destination.write_text(DEFAULT_MANIFEST, encoding="utf-8")
            _emit({"ok": True, "manifest": str(destination)}, args.json)
            return 0
        if args.command == "builtins":
            _emit({
                "builtins": [
                    {"name": name, "entry": entry}
                    for name, entry in sorted(BUILTIN_TOOLS.items())
                ]
            }, args.json)
            return 0
        if args.command == "doctor":
            return _doctor(args)

        manifest = load_manifest(_manifest_path(args))
        if args.command == "validate":
            _emit({
                "ok": True,
                "manifest": str(manifest.path),
                "tools": len(manifest.tools),
            }, args.json)
            return 0
        if args.command == "list":
            tools = [{
                "name": tool.name,
                "description": tool.description,
                "kind": "python" if tool.entry else "command",
                "cache": tool.cache,
                "inputs": list(tool.inputs),
                "outputs": list(tool.outputs),
                "parameters": {
                    name: {
                        "type": spec.type,
                        "required": spec.required,
                        **({"default": spec.default} if spec.has_default else {}),
                    }
                    for name, spec in tool.parameters.items()
                },
            } for tool in manifest.tools.values()]
            if args.json:
                _emit({"tools": tools}, True)
            else:
                for tool in tools:
                    suffix = f" — {tool['description']}" if tool["description"] else ""
                    print(f"{tool['name']} [{tool['kind']}]{suffix}")
            return 0
        if args.command == "run":
            reporter = Reporter(json_lines=args.json)
            runner = ToolRunner(
                manifest, reporter, saida_tool_override=args.saida_tool)
            result = runner.run(
                args.tool, _parameters(args.param), force=args.force)
            return 0 if result.ok else 1
        raise ManifestError(f"unsupported command '{args.command}'")
    except ManifestError as error:
        if args.json:
            _emit({"type": "result", "ok": False, "message": str(error)}, True)
        else:
            print(f"ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
