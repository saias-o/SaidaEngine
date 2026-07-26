"""Optional standalone authoring SDK for SaidaEngine projects."""

from .context import ToolContext
from .engine import SaidaTool, SaidaToolError
from .events import Diagnostic, Reporter
from .manifest import Manifest, ManifestError, ToolSpec, load_manifest
from .scene import (
    CollisionShapeType,
    SceneBuilder,
    SceneNode,
    ScriptBehaviour,
    Transform,
)

__all__ = [
    "CollisionShapeType",
    "Diagnostic",
    "Manifest",
    "ManifestError",
    "Reporter",
    "SaidaTool",
    "SaidaToolError",
    "SceneBuilder",
    "SceneNode",
    "ScriptBehaviour",
    "ToolContext",
    "ToolSpec",
    "Transform",
    "load_manifest",
]

__version__ = "0.1.0"
