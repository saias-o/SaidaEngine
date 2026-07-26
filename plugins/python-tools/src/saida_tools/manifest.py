"""Strict loader for the optional ``saida.tools.toml`` project manifest."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath, PureWindowsPath
import tomllib
from typing import Any


MANIFEST_SCHEMA = 1
PARAMETER_TYPES = {"string", "integer", "number", "boolean", "path"}


class ManifestError(ValueError):
    pass


@dataclass(frozen=True)
class ParameterSpec:
    name: str
    type: str = "string"
    description: str = ""
    required: bool = False
    default: Any = None
    has_default: bool = False

    def coerce(self, value: Any) -> Any:
        if self.type in {"string", "path"}:
            if not isinstance(value, str):
                value = str(value)
            return value
        if self.type == "integer":
            if isinstance(value, bool):
                raise ManifestError(f"parameter '{self.name}' expects an integer")
            try:
                return int(value)
            except (TypeError, ValueError) as error:
                raise ManifestError(
                    f"parameter '{self.name}' expects an integer") from error
        if self.type == "number":
            if isinstance(value, bool):
                raise ManifestError(f"parameter '{self.name}' expects a number")
            try:
                return float(value)
            except (TypeError, ValueError) as error:
                raise ManifestError(
                    f"parameter '{self.name}' expects a number") from error
        if self.type == "boolean":
            if isinstance(value, bool):
                return value
            text = str(value).strip().lower()
            if text in {"1", "true", "yes", "on"}:
                return True
            if text in {"0", "false", "no", "off"}:
                return False
            raise ManifestError(f"parameter '{self.name}' expects a boolean")
        raise ManifestError(f"unsupported parameter type '{self.type}'")


@dataclass(frozen=True)
class ToolSpec:
    name: str
    description: str = ""
    entry: str | None = None
    command: tuple[str, ...] | None = None
    inputs: tuple[str, ...] = ()
    outputs: tuple[str, ...] = ()
    cache: bool = True
    parameters: dict[str, ParameterSpec] = field(default_factory=dict)

    def resolve_parameters(self, supplied: dict[str, Any]) -> dict[str, Any]:
        unknown = sorted(set(supplied) - set(self.parameters))
        if unknown:
            raise ManifestError(
                f"tool '{self.name}' has no parameter(s): {', '.join(unknown)}")
        resolved: dict[str, Any] = {}
        for name, spec in self.parameters.items():
            if name in supplied:
                resolved[name] = spec.coerce(supplied[name])
            elif spec.has_default:
                resolved[name] = spec.coerce(spec.default)
            elif spec.required:
                raise ManifestError(
                    f"tool '{self.name}' requires parameter '{name}'")
        return resolved


@dataclass(frozen=True)
class Manifest:
    path: Path
    project_root: Path
    tools: dict[str, ToolSpec]
    saida_tool: str | None = None


_ROOT_KEYS = {"schema", "engine", "tools"}
_ENGINE_KEYS = {"saida_tool"}
_TOOL_KEYS = {
    "description", "entry", "command", "inputs", "outputs", "cache", "params",
}
_PARAM_KEYS = {"type", "description", "required", "default"}


def _reject_unknown(mapping: dict[str, Any], allowed: set[str], where: str) -> None:
    unknown = sorted(set(mapping) - allowed)
    if unknown:
        raise ManifestError(f"{where}: unknown field(s): {', '.join(unknown)}")


def _string_list(value: Any, where: str) -> tuple[str, ...]:
    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        raise ManifestError(f"{where} must be an array of strings")
    return tuple(value)


def _project_patterns(value: Any, where: str) -> tuple[str, ...]:
    patterns = _string_list(value, where)
    for pattern in patterns:
        normalized = pattern.replace("\\", "/")
        path = PurePosixPath(normalized)
        windows_path = PureWindowsPath(pattern)
        if (
            not pattern
            or path.is_absolute()
            or bool(windows_path.drive)
            or ".." in path.parts
        ):
            raise ManifestError(
                f"{where} entries must be project-relative patterns")
    return patterns


def _parameter(name: str, raw: Any, where: str) -> ParameterSpec:
    if not isinstance(raw, dict):
        raise ManifestError(f"{where} must be a table")
    _reject_unknown(raw, _PARAM_KEYS, where)
    kind = raw.get("type", "string")
    if kind not in PARAMETER_TYPES:
        raise ManifestError(
            f"{where}.type must be one of {', '.join(sorted(PARAMETER_TYPES))}")
    description = raw.get("description", "")
    required = raw.get("required", False)
    if not isinstance(description, str) or not isinstance(required, bool):
        raise ManifestError(f"{where} has invalid description/required fields")
    has_default = "default" in raw
    if required and has_default:
        raise ManifestError(f"{where} cannot be required and have a default")
    spec = ParameterSpec(
        name=name,
        type=kind,
        description=description,
        required=required,
        default=raw.get("default"),
        has_default=has_default,
    )
    if has_default:
        spec.coerce(spec.default)
    return spec


def _tool(name: str, raw: Any) -> ToolSpec:
    where = f"tools.{name}"
    if not isinstance(raw, dict):
        raise ManifestError(f"{where} must be a table")
    _reject_unknown(raw, _TOOL_KEYS, where)
    entry = raw.get("entry")
    command_raw = raw.get("command")
    if (entry is None) == (command_raw is None):
        raise ManifestError(f"{where} needs exactly one of 'entry' or 'command'")
    if entry is not None and (
            not isinstance(entry, str) or ":" not in entry or entry.startswith(":")):
        raise ManifestError(f"{where}.entry must be 'module:function'")
    command = None
    if command_raw is not None:
        command = _string_list(command_raw, f"{where}.command")
        if not command:
            raise ManifestError(f"{where}.command cannot be empty")
    description = raw.get("description", "")
    cache = raw.get("cache", True)
    if not isinstance(description, str) or not isinstance(cache, bool):
        raise ManifestError(f"{where} has invalid description/cache fields")
    inputs = _project_patterns(raw.get("inputs", []), f"{where}.inputs")
    outputs = _project_patterns(raw.get("outputs", []), f"{where}.outputs")
    params_raw = raw.get("params", {})
    if not isinstance(params_raw, dict):
        raise ManifestError(f"{where}.params must be a table")
    parameters = {
        param_name: _parameter(
            param_name, param_raw, f"{where}.params.{param_name}")
        for param_name, param_raw in params_raw.items()
    }
    return ToolSpec(
        name=name,
        description=description,
        entry=entry,
        command=command,
        inputs=inputs,
        outputs=outputs,
        cache=cache,
        parameters=parameters,
    )


def load_manifest(path: str | Path) -> Manifest:
    manifest_path = Path(path).resolve()
    try:
        with manifest_path.open("rb") as file:
            raw = tomllib.load(file)
    except (OSError, tomllib.TOMLDecodeError) as error:
        raise ManifestError(f"cannot read {manifest_path}: {error}") from error
    if not isinstance(raw, dict):
        raise ManifestError("manifest root must be a table")
    _reject_unknown(raw, _ROOT_KEYS, "manifest")
    if raw.get("schema") != MANIFEST_SCHEMA:
        raise ManifestError(
            f"manifest schema must be exactly {MANIFEST_SCHEMA}")

    engine = raw.get("engine", {})
    if not isinstance(engine, dict):
        raise ManifestError("engine must be a table")
    _reject_unknown(engine, _ENGINE_KEYS, "engine")
    saida_tool = engine.get("saida_tool")
    if saida_tool is not None and not isinstance(saida_tool, str):
        raise ManifestError("engine.saida_tool must be a string")

    tools_raw = raw.get("tools", {})
    if not isinstance(tools_raw, dict):
        raise ManifestError("tools must be a table")
    tools = {name: _tool(name, value) for name, value in tools_raw.items()}
    return Manifest(
        path=manifest_path,
        project_root=manifest_path.parent,
        tools=tools,
        saida_tool=saida_tool,
    )


def find_manifest(start: str | Path) -> Path | None:
    path = Path(start).resolve()
    if path.is_file():
        path = path.parent
    for candidate_root in (path, *path.parents):
        candidate = candidate_root / "saida.tools.toml"
        if candidate.is_file():
            return candidate
    return None
