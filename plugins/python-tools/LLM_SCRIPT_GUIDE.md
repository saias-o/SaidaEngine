# Python scripting guide for coding agents

This guide is the shortest reliable path for an LLM or contributor writing a
Python authoring script for a SaidaEngine project.

## The rule

A Saida Python tool is ordinary trusted Python. It may:

- import any package installed in its environment;
- invoke Blender, FFmpeg, ImageMagick or another executable;
- read source assets outside the project;
- use native Python, a virtual environment, Blender Python or another runtime;
- remain directly executable with its own `main()`.

The optional SDK adds discovery, typed parameters, diagnostics, caching and
transactional project outputs. It is not a sandbox and does not replace Python,
pip or external programs.

Do not add a dependency to the engine runtime merely because an authoring script
needs it. Keep script dependencies in the authoring environment.

## Choose the smallest integration

Use one of these three forms.

### 1. Plain script

Keep a normal script when it is a one-off operation or must run in a special
host such as Blender:

```sh
python tools/convert_assets.py
blender -b --python tools/bake_character.py -- source.fbx output.glb
```

No SDK import is required. This is a supported authoring workflow.

### 2. Python recipe

Use a recipe when the script produces project files and benefits from typed
parameters, cache receipts and rollback:

```python
def build_document(seed):
    return {"schema": 1, "seed": seed}


def run(context):
    context.write_json(
        "generated/data.json",
        build_document(context.params["seed"]),
    )


def main():
    # Keeping the old standalone entry point is fine.
    import json
    from pathlib import Path

    output = Path(__file__).parent / "generated" / "data.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(build_document(42), indent=2) + "\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
```

Declare it beside the `.saidaproj`:

```toml
schema = 1

[tools.generate-data]
description = "Generate project data"
entry = "tools.generate_data:run"
inputs = ["tools/generate_data.py", "source/**/*.json"]
outputs = ["generated/data.json"]
cache = true

[tools.generate-data.params.seed]
type = "integer"
default = 42
```

Run it explicitly:

```sh
saida-python-tools --project . run generate-data --param seed=7
```

### 3. External command

Use a command entry when the real tool is already a CLI. Arguments are passed
directly with `shell=False`; shell quoting is not part of the contract.

```toml
[tools.generate-power-ui]
description = "Extract HUD sprites with FFmpeg"
command = [
  "ffmpeg",
  "-y",
  "-i",
  "${param.source}",
  "${stage}/assets/ui/power.png",
]
outputs = ["assets/ui/power.png"]
cache = false

[tools.generate-power-ui.params.source]
type = "path"
required = true
```

The runner creates the parent directory of an exact staged output before
starting the command.

`${project}`, `${stage}` and `${param.NAME}` are available in command
arguments. Commands also receive `SAIDA_PROJECT_ROOT`, `SAIDA_STAGE_ROOT` and
`SAIDA_TOOL_NAME`.

## External packages and programs

Install project-specific libraries in the authoring environment:

```sh
python -m pip install pillow numpy trimesh
```

Import them normally. When a dependency is missing, fail with an actionable
message rather than silently producing reduced output:

```python
def run(context):
    try:
        from PIL import Image
    except ImportError as error:
        raise RuntimeError(
            "Pillow is required: python -m pip install pillow"
        ) from error

    image = Image.open(context.params["source"])
    image.save(context.output_path("assets/generated/image.png"))
```

Blender scripts normally run inside Blender's Python and do not need to import
the SDK. Wrap them as a command and send their output to `${stage}`:

```toml
[tools.bake-character]
command = [
  "blender",
  "-b",
  "--factory-startup",
  "--python",
  "${project}/tools/bake_character.py",
  "--",
  "${param.source}",
  "${stage}/assets/models/character.glb",
]
outputs = ["assets/models/character.glb"]
cache = false

[tools.bake-character.params.source]
type = "path"
required = true
```

## Cache rule for external inputs

Manifest `inputs` are intentionally project-relative. Their contents, the
recipe source, typed parameters and tool contract form the cache key.

If a source file lives outside the project, use `cache = false`. The parameter
value alone cannot prove whether that external file changed. Copying the source
into the project or implementing a domain-specific hash inside the recipe are
valid alternatives, but are not required.

Outputs should still use `${stage}` or `context.output_path(...)` so a failed
run cannot partially overwrite the project.

## Context cheat sheet

```python
def run(context):
    root = context.project_root
    mode = context.params["mode"]
    source = context.project_path("source/data.json")
    every_json = context.inputs("source/**/*.json")

    context.reporter.info(f"mode: {mode}")
    context.reporter.progress(1, len(every_json), "processing")
    context.reporter.diagnostic(
        "warning",
        "optional metadata was not found",
        file=source,
        code="metadata.missing",
    )

    context.write_text("generated/result.txt", "done\n")
    context.write_bytes("generated/result.bin", b"\x00\x01")
    context.write_json("generated/result.json", {"ok": True})
```

An `error` diagnostic, a `False` return value or an uncaught exception rejects
the run and rolls staged outputs back.

## Saida contracts

`context.saida_tool` is optional. Check availability before using it:

```python
if context.saida_tool.available:
    engine = context.saida_tool.describe_engine()
    report = context.saida_tool.validate_script("scripts/player.mjs")
```

Important format distinction:

- runtime `.scene` documents use numeric node IDs;
- authoring snapshots use decimal-string node IDs;
- `saida_tool validate-scene` validates an authoring snapshot, not a runtime
  `.scene`;
- the SDK exposes that command as `validate_authoring_snapshot()` to make the
  distinction explicit.

Use `SceneBuilder` for runtime `.scene` output.

## Checklist before declaring a script finished

1. Read the project's `README`, `AGENTS` and relevant specification.
2. Keep the generator as the source of truth; do not hand-edit generated output.
3. Prefer a plain script unless SDK discovery, parameters, cache or rollback add
   value.
4. Preserve an existing `main()` when adding `run(context)`.
5. Declare every project-relative input that changes the output.
6. Declare every project output and write it through staging.
7. Use `cache = false` for untracked external source files.
8. Fail explicitly when an external package or executable is missing.
9. Run `saida-python-tools validate`.
10. Run the tool twice and confirm the second run is cached when caching is
    enabled.
11. Run `project-audit`.
12. Never modify engine C++ merely to make a Python authoring workflow work;
    report the missing engine capability first.
