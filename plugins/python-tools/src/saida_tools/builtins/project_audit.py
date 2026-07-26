"""Read-only project consistency audit."""

from __future__ import annotations

import json
from pathlib import Path

from ..context import ToolContext
from ..engine import SaidaToolError


MAX_ISSUES_PER_FILE = 10


def _contained_project_file(context: ToolContext, relative: str) -> Path | None:
    candidate = (context.project_root / relative).resolve()
    if not candidate.is_relative_to(context.project_root):
        return None
    return candidate


def _load_json(path: Path, context: ToolContext) -> dict | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        context.reporter.diagnostic(
            "error", f"invalid JSON: {error}", file=path,
            code="json.invalid")
        return None
    if not isinstance(value, dict):
        context.reporter.diagnostic(
            "error", "document root must be an object", file=path,
            code="json.root")
        return None
    return value


def _runtime_scene_issues(
    document: dict,
    *,
    allowed_nodes: set[str] | None = None,
    allowed_behaviours: set[str] | None = None,
) -> list[tuple[str, str]]:
    """Validate the runtime SceneSerializer shape, not an authoring snapshot."""
    issues: list[tuple[str, str]] = []
    scene = document.get("scene")
    if not isinstance(scene, dict):
        return [("scene", "must be an object")]

    seen_ids: set[int] = set()

    def visit(node: object, path: str) -> None:
        if not isinstance(node, dict):
            issues.append((path, "node must be an object"))
            return

        node_id = node.get("id")
        if node_id is not None:
            if (
                isinstance(node_id, bool)
                or not isinstance(node_id, int)
                or node_id <= 0
                or node_id > (1 << 64) - 1
            ):
                issues.append(
                    (path, "'id' must be a non-zero uint64 JSON number"))
            elif node_id in seen_ids:
                issues.append((path, f"duplicate node id {node_id}"))
            else:
                seen_ids.add(node_id)

        node_type = node.get("type", "Node")
        if not isinstance(node_type, str):
            issues.append((path, "'type' must be a string"))
        elif allowed_nodes is not None and node_type not in allowed_nodes:
            issues.append((path, f"unsupported runtime node type '{node_type}'"))

        behaviours = node.get("behaviours", [])
        if not isinstance(behaviours, list):
            issues.append((path, "'behaviours' must be an array"))
        else:
            for index, behaviour in enumerate(behaviours):
                behaviour_path = f"{path}/behaviours[{index}]"
                if not isinstance(behaviour, dict):
                    issues.append((behaviour_path, "must be an object"))
                    continue
                behaviour_type = behaviour.get("type")
                if not isinstance(behaviour_type, str):
                    issues.append((behaviour_path, "'type' must be a string"))
                elif (
                    allowed_behaviours is not None
                    and behaviour_type not in allowed_behaviours
                ):
                    issues.append((
                        behaviour_path,
                        f"unsupported runtime behaviour type '{behaviour_type}'",
                    ))

        children = node.get("children", [])
        if not isinstance(children, list):
            issues.append((path, "'children' must be an array"))
        else:
            for index, child in enumerate(children):
                visit(child, f"{path}/children[{index}]")

    visit(scene, "scene")
    return issues


def run(context: ToolContext) -> bool:
    diagnostic_start = len(context.reporter.diagnostics)
    project_files = sorted(context.project_root.glob("*.saidaproj"))
    if len(project_files) != 1:
        context.reporter.diagnostic(
            "error",
            f"expected exactly one .saidaproj, found {len(project_files)}",
            file=context.project_root,
            code="project.count",
        )
        return False

    project_path = project_files[0]
    project = _load_json(project_path, context)
    if project is None:
        return False
    if project.get("schema") != 1 or project.get("version") != 1:
        context.reporter.diagnostic(
            "error", "project schema/version must both be 1",
            file=project_path, code="project.schema")
    main_scene = project.get("mainScene")
    if main_scene is not None:
        candidate = (
            _contained_project_file(context, main_scene)
            if isinstance(main_scene, str) else None
        )
        if candidate is None or not candidate.is_file():
            context.reporter.diagnostic(
                "error", f"mainScene is not a project file: {main_scene}",
                file=project_path, code="project.main_scene")

    registry_path = context.project_root / "asset_registry.json"
    if registry_path.is_file():
        registry = _load_json(registry_path, context)
        if registry:
            assets = registry.get("assets")
            if not isinstance(assets, dict):
                context.reporter.diagnostic(
                    "error", "assets must be an object", file=registry_path,
                    code="asset.registry_shape")
            else:
                for asset_id, metadata in assets.items():
                    relative = (
                        metadata.get("path")
                        if isinstance(metadata, dict) else None
                    )
                    candidate = (
                        _contained_project_file(context, relative)
                        if isinstance(relative, str) else None
                    )
                    if candidate is None or not candidate.is_file():
                        context.reporter.diagnostic(
                            "error",
                            f"asset {asset_id} has a missing path: {relative}",
                            file=registry_path, code="asset.missing")

    allowed_nodes = None
    allowed_behaviours = None
    native_ready = context.saida_tool.available
    if native_ready:
        try:
            engine = context.saida_tool.describe_engine()
            runtime_types = (
                engine.get("runtimeTypeMatrix", {}).get("types", [])
                if isinstance(engine.get("runtimeTypeMatrix"), dict) else []
            )
            allowed_nodes = {
                item["name"] for item in engine.get("nodes", [])
                if isinstance(item, dict) and isinstance(item.get("name"), str)
            }
            allowed_behaviours = {
                item["name"] for item in engine.get("behaviours", [])
                if isinstance(item, dict) and isinstance(item.get("name"), str)
            }
            for item in runtime_types:
                if not isinstance(item, dict):
                    continue
                name = item.get("name")
                support = item.get("support")
                if (
                    not isinstance(name, str)
                    or not isinstance(support, dict)
                    or support.get("native") == "absent"
                ):
                    continue
                if item.get("category") == "node":
                    allowed_nodes.add(name)
                elif item.get("category") == "behaviour":
                    allowed_behaviours.add(name)
        except SaidaToolError as error:
            context.reporter.diagnostic(
                "error", str(error), code="saida_tool.failed")
            native_ready = False

    for scene_path in sorted((context.project_root / "scenes").rglob("*.scene")):
        scene = _load_json(scene_path, context)
        if scene and (scene.get("schema") != 2 or scene.get("version") != 2):
            context.reporter.diagnostic(
                "error", "runtime scene schema/version must both be 2",
                file=scene_path, code="scene.schema")
        if scene:
            issues = _runtime_scene_issues(
                scene,
                allowed_nodes=allowed_nodes,
                allowed_behaviours=allowed_behaviours,
            )
            for location, message in issues[:MAX_ISSUES_PER_FILE]:
                context.reporter.diagnostic(
                    "error", f"{location}: {message}", file=scene_path,
                    code="scene.runtime")
            remaining = len(issues) - MAX_ISSUES_PER_FILE
            if remaining > 0:
                context.reporter.diagnostic(
                    "error",
                    f"{remaining} additional runtime scene issue(s) omitted",
                    file=scene_path, code="scene.runtime.truncated")

    if native_ready:
        for script in sorted((context.project_root / "scripts").rglob("*.mjs")):
            try:
                report = context.saida_tool.validate_script(script)
                if not report.get("ok", False):
                    context.reporter.diagnostic(
                        "error", report.get("error", "script validation failed"),
                        file=script, code="script.invalid")
            except SaidaToolError as error:
                context.reporter.diagnostic(
                    "error", str(error), file=script, code="saida_tool.failed")
                # A broken native validator is not a warning: the audit asked
                # for it because it was discoverable.
                break
    elif not context.saida_tool.available:
        context.reporter.diagnostic(
            "warning",
            "saida_tool not found; JavaScript syntax validation was skipped",
            code="saida_tool.missing",
        )

    return not any(
        item.severity == "error"
        for item in context.reporter.diagnostics[diagnostic_start:])
