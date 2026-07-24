# Instructions for LLM agents

These instructions apply to every automated coding agent working in this
repository.

## Canonical sources

1. Read [SPEC.md](SPEC.md) before changing architecture, contracts, formats,
   platform behavior or release procedures.
2. Read [ROADMAP.md](ROADMAP.md) before selecting or closing work.
3. Follow [CONTRIBUTING.md](CONTRIBUTING.md) for the development and
   verification workflow.
4. Treat `SPEC.md` as the truth of what exists and `ROADMAP.md` as the truth of
   what remains to be done.
5. Do not create competing planning or specification documents.

## Authorship

- Never add an LLM agent, model, vendor or agent tool as a commit co-author.
- Never add a `Co-authored-by` trailer for an AI agent.
- Do not claim human authorship or alter the configured human/service Git
  identity.

## Language

- Write all documentation, code comments, commit messages and user-facing
  repository text in English.
- Treat non-English documentation or comments as defects when they are in the
  scope of the current change.

## Change discipline

- Preserve unrelated user changes and work safely in a dirty worktree.
- Make focused changes; do not perform unrelated refactors.
- Update `ROADMAP.md` when a roadmap item is closed or a new proven blocker is
  introduced.
- Update `SPEC.md` when the implemented technical truth or a public contract
  changes.
- Do not weaken, delete or bypass tests merely to make a change pass.
- Do not edit vendored sources under `third_party`.
- Do not add silent fallbacks for missing capabilities or invalid durable
  content.
- Do not declare support without a real backend and an associated proof.

## Architecture and code quality

- Keep the editor, desktop, Web, XR and headless surfaces on the shared scene
  and gameplay model.
- Prefer RAII, explicit ownership and small components with clear
  responsibilities.
- Reject duplicated engine logic, hidden dependencies, unjustified global
  state and permanent magic values.
- Comments explain invariants, external constraints or non-obvious decisions;
  they do not narrate straightforward code.

## Formats and generated content

- Durable documents require the exact current schema and version.
- A format change updates its producers, loaders, fixtures and tests together.
- Never regenerate a frozen fixture merely to hide a divergence.
- Modify Witness generators rather than generated Witness outputs when the
  generator is the source of truth.
- Do not commit build directories, caches, crash reports, credentials or
  signing material.

## Verification

Run checks proportional to the change and report exactly what was run.

The native baseline is:

```sh
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

After a change to snapshots, SaidaOps, manifests, registries, scripting or
shared input, also rebuild and verify authoring WASM and the Web player.

Use the Witness harnesses for changes affecting export, runtime behavior,
gameplay, UI, persistence or restart:

```sh
./tools/witness_e2e.sh
./tools/witness_editor_play.sh
./tools/witness_editor_build.sh
./tools/witness_web_stage.sh
```

Do not leave GUI applications or local servers running after verification.

## Release handling

- Release identities are immutable tags, commit SHAs, manifests and digests.
- Never move, overwrite or rebuild an existing beta, RC or stable tag.
- `latest` is only a convenience alias, never the release identity.
- Do not publish a stable release or claim Windows qualification without the
  required proofs and Authenticode verification.
- Record incomplete manual testing honestly in pre-release notes.
