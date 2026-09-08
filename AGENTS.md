# Instructions for LLM agents

These instructions apply to every automated coding agent working in this
repository. The contributor workflow and the engine documentation apply in
full; this file contains only agent-specific rules.

## Canonical sources

1. Read [SPEC.md](SPEC.md) before changing architecture, contracts, formats,
   platform behavior or release procedures.
2. Read [ROADMAP.md](ROADMAP.md) before selecting or closing work.
3. Follow [CONTRIBUTING.md](CONTRIBUTING.md) for development, verification,
   generated-content, asset and release rules.
4. Treat `SPEC.md` as the truth of what exists and `ROADMAP.md` as the truth of
   what remains to be done. Do not create competing planning or specification
   documents.

Place durable knowledge in its canonical document, never in this file:

- current engine behavior, public contracts and known technical limits belong
  in `SPEC.md`;
- open work and proven blockers belong in `ROADMAP.md`;
- setup, contribution, troubleshooting and verification procedures belong in
  `CONTRIBUTING.md`;
- details specific to a sample or game belong in that project's `README.md`.

## Authorship

- Never add an LLM agent, model, vendor or agent tool as a commit co-author.
- Never add a `Co-authored-by` trailer for an AI agent.
- Do not claim human authorship or alter the configured human or service Git
  identity.

## Two local failures that are not what they look like

Both cost an agent significant time by reading as regressions in the change
under test. Neither is.

**A bare `ld returned 116`.** The build in `build/` is UCRT64. If `/mingw64/bin`
precedes `/c/msys64/ucrt64/bin` on PATH, binaries resolve `libstdc++-6.dll` and
`libgcc_s_seh-1.dll` from the wrong runtime, and two failures appear together:
`SaidaEngineRuntime.exe` fails to link with `collect2.exe: error: ld returned
116 exit status` and **no other diagnostic** — it dies writing the import
library, and `SaidaEngine.exe` never builds either because it depends on it —
while `saida_core_tests`, `saida_png_writer_tests`,
`saida_gpu_driven_contract_tests` and `saida_exe_metadata_tests` fail under
CTest with `0xC0000139` (STATUS_ENTRYPOINT_NOT_FOUND). Put
`/c/msys64/ucrt64/bin` first on PATH before cmake, ninja, ctest and anything run
from `build/bin`. Confirm with `ldd build/bin/<exe> | grep libstdc`: it must
resolve under `/c/msys64/ucrt64/bin`. Check this before suspecting the change or
a toolchain limit — the failure carries no message pointing at its cause.

**`[E2E] FAIL: no relic collected within 25s` on a first run.**
`tools/witness_e2e.sh` and `tools/witness_editor_play.sh` budget 25 s of wall
clock for the driver, and the first launch after a fresh export or rebuild also
pays for cold shader/pipeline cache compilation. Re-run the harness before
investigating; treat it as a regression only if it repeats on a warm cache. When
reporting, give the run count rather than quoting the passing run alone.
