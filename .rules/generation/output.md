# Output: Commits, Documentation, Workflow

## Git Convention (MANDATORY)

After every completed task:
1. `git add` — changed files only (never `git add .` blindly)
2. `git commit` — conventional commits, Ukrainian body
3. `git push origin main` — after confirmation

### Pre-commit check
Before EVERY commit: verify `git diff --cached` contains NO secrets (passwords, tokens, keys, certs, .env values).

### Commit message format
```
feat(module): short description

- Detail 1
- Detail 2
```

### Prefixes
| Prefix | When |
|---|---|
| `feat(module)` | New feature or capability |
| `fix(module)` | Bug fix |
| `refactor(module)` | Code restructure without behavior change |
| `perf(module)` | Performance improvement |
| `docs` | Documentation only |
| `test` | Tests only |
| `chore` | Build, config, maintenance |
| `release` | Version bump, release prep |

## Documentation Updates

| File | What | When to update |
|---|---|---|
| `docs/CHANGELOG.md` | Full changelog | After each feature/fix |
| `docs/*.md` (01-12) | Technical docs per area | When that area changes |
| `README.md` | Project overview + metrics | Major changes, release |
| `docs/FEATURES.md` | Feature list | New features |

### Rule: Documentation = mirror of code
- If feature works — it's documented
- If feature doesn't work — not documented as ready
- Don't describe in docs what doesn't exist in code

## Build & Deploy

| Action | Command |
|---|---|
| ESP-IDF build | `powershell -ExecutionPolicy Bypass -File run_build.ps1` |
| Flash + monitor | `idf.py -p COM15 flash monitor` |
| WebUI build | `cd webui && npm run build` |
| WebUI deploy | `cd webui && npm run deploy` |
| Run generator | `python tools/generate_ui.py` |
| Run pytest | `cd tools && python -m pytest tests/ -v` |
| Run host tests | Build + run from `tests/host/build/` |

## Response Style

- Comments in code: **Ukrainian**
- Doxygen: **English**
- Commit messages: prefix English, body Ukrainian
- Communication with user: match user's language
