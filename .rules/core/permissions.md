# Permissions: Access Control

## Three-Tier Model

### Allowed — autonomous, no confirmation needed

| Area | Files |
|---|---|
| Firmware code | `components/*/src/`, `modules/*/src/`, `drivers/*/src/`, `main/` |
| Firmware headers | `components/*/include/`, `modules/*/include/`, `drivers/*/include/` |
| Manifests | `modules/*/manifest.json`, `drivers/*/manifest.json` |
| Board configs | `data/board.json`, `data/bindings.json`, `boards/` |
| WebUI source | `webui/src/` |
| i18n | `data/i18n/`, `modules/*/i18n/`, `webui/src/i18n/` |
| Tools & scripts | `tools/` (except `generate_ui.py` core logic) |
| Tests | `tests/host/`, `tools/tests/` |
| Docs | `docs/`, `README.md` |

### Ask First — confirm with user before proceeding

| Action | Why |
|---|---|
| Edit `.rules/`, `CLAUDE.md` | Changes agent behavior globally |
| Edit `partitions.csv` | Wrong partition = bricked device |
| Edit root `CMakeLists.txt` | Breaks build for all components |
| Delete any file | Irreversible |
| Change build config (`sdkconfig*`) | Side effects on all modules |
| Push to remote | Shared state, may trigger CI |
| Create new component/module | Architecture decision |
| Edit `Kconfig.projbuild` | Affects menuconfig for all users |

### Forbidden — NEVER do this

| Action | Why |
|---|---|
| Commit secrets (keys, passwords, certs, `.env`) | Permanent exposure in git history |
| Edit files in `generated/` | Overwritten by `generate_ui.py` every build |
| Edit `data/ui.json` | Generated from manifests |
| Edit `data/www/bundle.*` | Built from `webui/` source |
| Direct HAL access from business modules | Only Equipment touches HAL drivers |
| `std::string`/`new`/`malloc` in on_update/on_message | Heap fragmentation on ESP32 |

## Role-Specific Boundaries

Each role has a defined scope — see `context/roles.md`.
A role MUST NOT write files outside its scope without explicit user approval.
