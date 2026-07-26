"""Small typed builder for SaidaEngine runtime scene documents."""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import IntEnum
import hashlib
from typing import Any, Iterable


MAX_NODE_ID = (1 << 64) - 1


class CollisionShapeType(IntEnum):
    AUTO = 0
    BOX = 1
    SPHERE = 2
    CAPSULE = 3
    HULL = 4
    MESH = 5


def stable_node_id(name: str) -> int:
    value = int.from_bytes(
        hashlib.sha256(name.encode("utf-8")).digest()[:8], "big")
    return value or 1


def _node_id_value(value: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"runtime node id must be an integer: {value}")
    if value == 0 or value > MAX_NODE_ID:
        raise ValueError(f"node id is outside the uint64 range: {value}")
    return value


@dataclass(frozen=True)
class Transform:
    position: tuple[float, float, float] = (0.0, 0.0, 0.0)
    rotation: tuple[float, float, float, float] = (0.0, 0.0, 0.0, 1.0)
    scale: tuple[float, float, float] = (1.0, 1.0, 1.0)

    def to_dict(self) -> dict[str, list[float]]:
        return {
            "position": list(self.position),
            "rotation": list(self.rotation),
            "scale": list(self.scale),
        }


@dataclass(frozen=True)
class ScriptBehaviour:
    script: str
    properties: dict[str, Any] = field(default_factory=dict)
    enabled: bool = True
    hot_reload: bool = True

    def to_dict(self) -> dict[str, Any]:
        return {
            "type": "ScriptBehaviour",
            "enabled": self.enabled,
            "script": self.script,
            "hotReload": self.hot_reload,
            "properties": self.properties,
        }


@dataclass
class SceneNode:
    type: str
    name: str
    transform: Transform = field(default_factory=Transform)
    enabled: bool = True
    behaviours: list[ScriptBehaviour | dict[str, Any]] = field(default_factory=list)
    children: list["SceneNode"] = field(default_factory=list)
    groups: list[str] = field(default_factory=list)
    properties: dict[str, Any] = field(default_factory=dict)
    id: int | None = None

    def add(self, *children: "SceneNode") -> "SceneNode":
        self.children.extend(children)
        return self

    def to_dict(self) -> dict[str, Any]:
        result = {
            "type": self.type,
            "id": (
                _node_id_value(self.id)
                if self.id is not None else stable_node_id(self.name)
            ),
            "name": self.name,
            "enabled": self.enabled,
            "behaviours": [
                item.to_dict() if isinstance(item, ScriptBehaviour) else item
                for item in self.behaviours
            ],
            "transform": self.transform.to_dict(),
            **self.properties,
        }
        if self.groups:
            result["groups"] = self.groups
        result["children"] = [child.to_dict() for child in self.children]
        return result


class SceneBuilder:
    def __init__(self, name: str, *, schema: int = 2):
        self.schema = schema
        self.root = SceneNode("Scene", name)
        self.settings: dict[str, Any] = {}

    def add(self, *nodes: SceneNode) -> "SceneBuilder":
        self.root.add(*nodes)
        return self

    def node(self, type: str, name: str, *,
             transform: Transform | None = None,
             behaviours: Iterable[ScriptBehaviour | dict[str, Any]] = (),
             children: Iterable[SceneNode] = (),
             groups: Iterable[str] = (), **properties: Any) -> SceneNode:
        return SceneNode(
            type=type,
            name=name,
            transform=transform or Transform(),
            behaviours=list(behaviours),
            children=list(children),
            groups=list(groups),
            properties=properties,
        )

    def document(self) -> dict[str, Any]:
        root = self.root.to_dict()
        root["settings"] = self.settings
        seen: set[int] = set()

        def visit(node: dict[str, Any]) -> None:
            node_id = node["id"]
            if node_id in seen:
                raise ValueError(f"duplicate scene node id {node_id}")
            seen.add(node_id)
            for child in node.get("children", []):
                visit(child)

        visit(root)
        return {"schema": self.schema, "version": self.schema, "scene": root}
