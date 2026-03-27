# Meta

## Version: 2.0 — 2026-03-21
## Sizing: Knowledge=L x Surfaces=2 x Extensions=1

## Files:

| File | Purpose |
|---|---|
| `_index.md` | Identity, permissions summary, role selection, context loading |
| `_meta.md` | Version, sizing, file list, changelog |
| `core/constraints.md` | Zero heap, ETL, ESP-IDF style, SSoT, secrets protection |
| `core/architecture.md` | System overview, modules, services, drivers |
| `core/permissions.md` | Three-tier access control, role boundaries |
| `context/task.md` | Current goal: release preparation |
| `context/roles.md` | 7 agent roles with scope, triggers, escalation |
| `context/references.md` | API endpoints, project structure, build env |
| `generation/patterns.md` | Code patterns with examples (ETL, SharedState, FreeRTOS) |
| `generation/output.md` | Commit convention, documentation rules, build commands |
| `quality/eval.md` | Quality checklist, red flags, release readiness |

## Adapters:

| File | Surface |
|---|---|
| `AGENTS.md` | Universal hub (Copilot, Codex, Amp, etc.) |
| `.github/copilot-instructions.md` | VS Code GitHub Copilot |
| `.claude/commands/review.md` | Claude Code slash command |

## Changelog:
- 2026-03-21 — v2.0: 7 agent roles, secrets protection, release readiness checklist, AGENTS.md + Copilot adapter, review slash command
- 2026-03-14 — v1.0: Initial creation from 547-line CLAUDE.md
