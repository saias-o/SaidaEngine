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
