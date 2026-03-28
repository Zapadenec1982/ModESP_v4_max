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
| `context/memory.md` | Decisions, errors, workarounds (cross-session) |
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

## Effectiveness Tracking

| Date | Request | Pass/Fail | Rule | Action |
|---|---|---|---|---|
| | | | | |

> After each session — 1 line. Target: ≥ 85% pass. < 70% → review.

## Changelog:
- 2026-03-28 — v2.2: Full code review — architecture updated with EEV (PI, 7 states, 23 refrigerants, 4 valve drivers), Modbus RTU, Lighting, multi-zone, pressure_adc. 7 modules not 5. Task updated with MPXPRO comparison phases. References corrected.
- 2026-03-28 — v2.1: Audit fixes — memory.md, WHY in architecture, metrics update, i18n sync, rollback cmd, snippets in AGENTS.md
- 2026-03-21 — v2.0: 7 agent roles, secrets protection, release readiness checklist, AGENTS.md + Copilot adapter, review slash command
- 2026-03-14 — v1.0: Initial creation from 547-line CLAUDE.md
