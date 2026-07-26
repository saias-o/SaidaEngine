# Saida Python Tools

`saida-python-tools` is an optional, standalone Python authoring SDK for
SaidaEngine projects. It makes project generators, converters, audits and asset
pipelines reproducible without embedding Python in the engine.

The separation is intentional:

- the engine, editor and exported players never import or load this package;
- the package is absent from CMake and does not change a `.saidaproj`;
- projects run exactly as before when Python or this package is not installed;
- tools run only after an explicit CLI invocation;
- the core package has no third-party Python dependency;
- `saida_tool` is an optional subprocess adapter, not a Python dependency.

Project recipes are trusted authoring code. The manifest, cache and staging
rules provide validation and reproducibility, not an operating-system sandbox.
Recipes may import any library or invoke any external program available in
their own environment. The SDK coordinates tools; it does not restrict them.

For a concise workflow aimed at coding agents, including copy-paste recipes,
Blender/FFmpeg examples and a completion checklist, read the
[LLM Python scripting guide](LLM_SCRIPT_GUIDE.md).

## Install

Python 3.11 or newer is required only on machines that run authoring tools.

```sh
python -m venv .venv
.venv/Scripts/python -m pip install -e plugins/python-tools
```

On Linux or macOS, use `.venv/bin/python` instead. Installing from a detached
copy or a built wheel works as well; the package does not require an engine
source checkout.

Initialize a project only when it opts in:

```sh
saida-python-tools --project ../MyGame init
saida-python-tools --project ../MyGame validate
saida-python-tools --project ../MyGame list
saida-python-tools --project ../MyGame doctor
```

`init` creates `saida.tools.toml` beside the project's `.saidaproj`. It does not
edit the Saida project file.

## Manifest

The exact version-1 manifest is deliberately small:

```toml
schema = 1

# Optional. SAIDA_TOOL and PATH are checked as well.
[engine]
saida_tool = "../saidaengine/build/bin/saida_tool.exe"

[tools.generate-level]
description = "Generate a runtime scene from project-local source data"
entry = "tools.generate_level:run"
inputs = ["source/**/*.json", "tools/generate_level.py"]
outputs = ["scenes/generated.scene"]
cache = true

[tools.generate-level.params.seed]
type = "integer"
required = true
description = "Deterministic generation seed"

[tools.optimize-model]
description = "Wrap an existing executable without invoking a shell"
command = [
  "model-optimizer",
  "${project}/assets/source.glb",
  "${stage}/assets/generated/source.glb",
]
inputs = ["assets/source.glb"]
outputs = ["assets/generated/source.glb"]
cache = true
```

Every tool has exactly one execution form:

- `entry = "module:function"` imports a Python recipe from the project or an
  installed package;
- `command = ["executable", "argument", ...]` starts an argument vector with
  `shell=False`.

Supported parameter types are `string`, `integer`, `number`, `boolean` and
`path`. A parameter can be `required = true` or have a `default`, but not both.
Unknown fields, unknown parameters and undeclared outputs are rejected.

Command arguments may contain `${project}`, `${stage}` and
`${param.parameterName}`. Command tools also receive `SAIDA_PROJECT_ROOT`,
`SAIDA_STAGE_ROOT` and `SAIDA_TOOL_NAME`.

## Write a Python recipe

```python
from saida_tools import SceneBuilder, Transform


def run(context):
    scene = SceneBuilder("GeneratedLevel")
    scene.add(scene.node(
        "Node",
        "Spawn",
        transform=Transform(position=(0.0, 2.0, 0.0)),
    ))
    context.write_json("scenes/generated.scene", scene.document())
```

The callable receives a `ToolContext`:

- `context.project_root` is the absolute project directory;
- `context.params` contains validated and converted parameters;
- `context.inputs(pattern)` returns deterministic input paths;
- `context.project_path(path)` resolves a safe project-relative path;
- `context.output_path`, `write_text`, `write_bytes` and `write_json` write to
  the private staging directory;
- `context.reporter` emits progress, diagnostics and artifacts;
- `context.saida_tool` exposes the optional native contract.

A `SceneBuilder` emits canonical schema/version 2 runtime `.scene` documents
with numeric 64-bit node IDs. Runtime scenes are deliberately distinct from
authoring snapshots, whose node IDs are decimal strings.

A recipe may return `False` or emit an `error` diagnostic to reject the run.
Uncaught exceptions also reject it.

## Native adapter

When `saida_tool` is available, recipes can use the engine's native source of
truth without linking to the engine:

```python
if context.saida_tool.available:
    engine = context.saida_tool.describe_engine()
    snapshot_report = context.saida_tool.validate_authoring_snapshot(
        "authoring/snapshot.json")
    script_report = context.saida_tool.validate_script("scripts/player.mjs")
```

The adapter also exposes `validate_ops`, `validate_scenario` and
`inspect_animation`. `validate_authoring_snapshot` wraps the native command
named `validate-scene`; it must not be used for runtime `.scene` files. The
adapter locates the executable from, in order, the manifest or CLI override,
`SAIDA_TOOL`, `PATH`, a local engine checkout, or a sibling `saidaengine`
checkout. Recipes that do not call it remain fully usable without the native
executable.

## Execution guarantees

Inputs, parameters, the tool contract and a Python entry file are hashed into a
content key. A cached tool is skipped only when its receipt matches and every
declared output still exists. Use `--force` to bypass the cache.

Outputs are first written below `.saida/python-tools/staging/`. Commit replaces
declared project files atomically per file and restores previous files if a
later replacement fails. Receipts live below `.saida/python-tools/cache/`.
Neither directory is part of the game format or an export.

The output declaration is a write boundary for the transaction. Python recipes
are still normal trusted Python and can access the host outside the context
API, so manifests from untrusted projects must not be executed.

## Built-in recipes

`saida-python-tools builtins` lists reusable entry points:

- `saida_tools.builtins.project_audit:run` checks the project envelope,
  referenced assets and runtime scene structure, uses the optional engine
  manifest to check native node/behaviour types, and delegates JavaScript
  syntax validation to `saida_tool` when it is present;
- `saida_tools.builtins.asset_inventory:run` writes a deterministic,
  SHA-256-addressed asset inventory without loading whole assets into memory.

`saida-python-tools init` declares both tools. The complete project-local
example under [`examples/minimal`](examples/minimal) also demonstrates a custom
recipe.

## Automation

Place `--json` before the command to emit JSON or JSONL:

```sh
saida-python-tools --json --project ../MyGame validate
saida-python-tools --json --project ../MyGame run generate-level --param seed=42
```

Tool runs emit `info`, `progress`, `diagnostic`, `artifact` and final `result`
events. Exit status is `0` for success, `1` for a failed tool and `2` for a CLI
or manifest error.

## Develop and verify

From the engine repository:

```sh
set PYTHONPATH=plugins\python-tools\src
python -m unittest discover -s plugins\python-tools\tests -v
python -m saida_tools --project plugins/python-tools/examples/minimal validate
```

On POSIX shells, use
`PYTHONPATH=plugins/python-tools/src python -m unittest ...`.

The package deliberately does not provide editor auto-run, background watchers
or an embedded interpreter. Those would couple project execution to Python and
need a separate product decision.
