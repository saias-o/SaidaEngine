"""Deterministic asset inventory recipe."""

from __future__ import annotations

import hashlib

from ..context import ToolContext


def _sha256(path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        while chunk := file.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def run(context: ToolContext) -> None:
    output = context.params.get("output", "reports/asset-inventory.json")
    assets_root = context.project_root / "assets"
    files = []
    paths = sorted(
        (path for path in assets_root.rglob("*") if path.is_file()),
        key=lambda path: path.as_posix(),
    ) if assets_root.is_dir() else []
    total = len(paths)
    for index, path in enumerate(paths, 1):
        relative = path.relative_to(context.project_root).as_posix()
        files.append({
            "path": relative,
            "bytes": path.stat().st_size,
            "sha256": _sha256(path),
        })
        context.reporter.progress(index, total, relative)
    context.write_json(output, {
        "schema": 1,
        "count": len(files),
        "bytes": sum(item["bytes"] for item in files),
        "assets": files,
    })
