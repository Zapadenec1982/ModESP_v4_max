# ModESP v4: Agent Team

## Identity

- **Domain:** Industrial refrigeration controllers (ESP32)
- **Stack:** ESP-IDF 5.5, C++17, ETL (Embedded Template Library), Svelte 4
- **Hardware:** ESP32-WROOM-32, 4MB flash
- **Language:** Code comments — Ukrainian, Doxygen — English, UI — UA/EN
- **Mode:** Team of 7 agents — see `context/roles.md`

## Instruction Hierarchy

1. User request (highest)
2. `.rules/` core rules
3. CLAUDE.md project-specific
4. Default agent behavior (lowest)

## Priority

safety > constraints > task > architecture > style

## Permissions

See `core/permissions.md` for full model.

| | Actions |
|---|---|
| **Allowed** | Edit `components/`, `modules/*/src/`, `drivers/*/src/`, `webui/src/`, `tools/`, `main/`, manifests, `data/board.json`, `data/bindings.json`, `tests/`, `docs/` |
| **Ask First** | `.rules/`, `CLAUDE.md`, `partitions.csv`, root `CMakeLists.txt`, delete files, change build config, push to remote |
| **Forbidden** | Commit secrets (`.env`, API keys, tokens, certs), edit `generated/`, `data/ui.json`, `data/www/bundle.*` |

## Response Contract

- New file: full content
- Modification: minimal diff with context
- Max 1-2 files per response unless task requires more
- Always show what changed and why

## Anti-Overengineering

- Simple > clever; extend existing > create new
- Don't add features/refactoring beyond what was asked
- Don't add error handling for impossible scenarios
- Three similar lines > premature abstraction

## Role Selection

Each request → pick the best-fit role from `context/roles.md`.
If unclear → default to **Firmware Dev** for C++, **WebUI Dev** for Svelte, **Docs Writer** for docs.

## Context Loading

When working on specific areas, read the relevant `.rules/` file:

| Task area | Read |
|---|---|
| Writing C++ code | `core/constraints.md`, `generation/patterns.md` |
| Architecture decisions | `core/architecture.md` |
| API, state keys, protocols | `context/references.md` |
| Code patterns, examples | `generation/patterns.md` |
| Commits, documentation | `generation/output.md` |
| Reviewing changes | `quality/eval.md` |
| Role scope, boundaries | `context/roles.md`, `core/permissions.md` |
