# Roles: Agent Team

## Role Definitions

### Firmware Dev
- **Scope:** `components/`, `modules/*/src/`, `drivers/*/src/`, `main/`, manifests
- **Reads:** architecture, constraints, patterns, references
- **Writes:** C++ code, headers, manifests, CMakeLists within components
- **Trigger:** New features, bug fixes, module changes, driver work
- **Escalate:** Architecture changes → user; HAL changes → check Equipment arbitration

### WebUI Dev
- **Scope:** `webui/src/`, `data/i18n/`
- **Reads:** patterns, references (API, WS protocol)
- **Writes:** Svelte components, JS/CSS, i18n files
- **Trigger:** UI changes, new pages/widgets, i18n, WebSocket integration
- **Escalate:** New HTTP endpoints → Firmware Dev; design decisions → Designer

### Designer
- **Scope:** `webui/src/components/`, `webui/src/pages/`, CSS/styles
- **Reads:** patterns (UI), existing component library
- **Writes:** Svelte components (visual/layout), CSS custom properties, design tokens
- **Trigger:** UX/UI improvements, layout changes, theming, responsive design
- **Constraint:** Maintain existing dark theme system; bundle must stay < 80KB gzipped
- **Escalate:** New component architecture → WebUI Dev

### Reviewer
- **Scope:** entire project (READ-ONLY)
- **Reads:** eval, constraints, permissions, all source
- **Writes:** nothing — produces review comments only
- **Trigger:** Before commit/merge, code review requests
- **Checklist:** `quality/eval.md`

### Tester
- **Scope:** `tests/host/`, `tools/tests/`
- **Reads:** eval, constraints, patterns
- **Writes:** test files, test fixtures, conftest
- **Trigger:** After code changes, new features, bug fixes
- **Constraint:** Host tests use doctest; Python tests use pytest
- **Escalate:** Test infrastructure changes → user

### Docs Writer
- **Scope:** `docs/`, `README.md`, `docs/CHANGELOG.md`
- **Reads:** output, references, architecture, all source (read-only)
- **Writes:** Markdown documentation only
- **Trigger:** After feature completion, release prep, API changes
- **Rule:** Documentation = mirror of code. Don't document what doesn't exist.

### Release Engineer
- **Scope:** build scripts, docs, version info, `sdkconfig.defaults`
- **Reads:** all rules, all source
- **Writes:** docs, version configs, changelog, build scripts
- **Trigger:** Release preparation, version bump, build verification
- **Checklist:** `quality/eval.md` → release readiness section
- **Escalate:** Build failures → Firmware Dev; UI issues → WebUI Dev

## Selection Logic

| Request pattern | Role |
|---|---|
| C++ code, modules, drivers, sensors | Firmware Dev |
| Svelte, UI components, pages, widgets | WebUI Dev |
| Layout, colors, UX, responsive, theme | Designer |
| "Review", "check", "перевір" | Reviewer |
| Tests, coverage, assertions | Tester |
| Documentation, changelog, README | Docs Writer |
| Build, flash, release, version | Release Engineer |
| Ambiguous | Ask user or pick by file type |
