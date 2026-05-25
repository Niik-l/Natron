# Project Context for Claude

This is a fork of Natron (https://github.com/Niik-l/Natron) with custom 3D system, Cycles renderer, deep compositing, particle system, and more.

## Build Workflow

See `BUILDING.md` for full instructions. Quick reference:

```bash
# In MSYS2 MINGW64 terminal, from the repo root
export LLVM_INSTALL_DIR=C:/msys64/mingw64
cd build-qt6
mingw32-make -j2
cd App && ./Natron.exe
```

If you add a new source file or rename one, run `cmake .. -G "MinGW Makefiles"` first to regenerate build files.

## Git Workflow — IMPORTANT RULES

**Read `GIT_WORKFLOW.md` for full details.** Key rules:

1. **Direct commits to `RB-2.6` are fine for small verified fixes / incremental features** — this is a solo fork, not a team repo. Use a `feature/*` or `experiment/*` branch only for multi-week refactors or risky work that might be thrown away. If/when collaborators join, default back to feature branches for everything.
2. **NEVER rewrite published history** — no `git rebase -i`, no `git push --force` on pushed commits. Amending a local-only commit before pushing is fine.
3. **NEVER commit build artifacts** — `build-qt6/` is gitignored, keep it that way
4. **Logical commits** — one commit per logical change, not lumps of unrelated work
5. **Branch naming (when you do use one):** `feature/short-name`, `fix/short-name`, `docs/short-name` (kebab-case)
6. **Commit anytime, push only after work hours** — Claude commits freely; the user runs `git push` themselves when they're off the clock.

When the user asks to commit:
- Check `git status --short | grep -v "build-qt6/"` to see real changes
- Group related files into logical commits
- Write clear commit messages: `<area>: <summary>` with a `Co-Authored-By: Claude` trailer (bare — no email, no version)
- Don't push — the user does that themselves

## Project Structure

- `Engine/` — core engine, render nodes
- `Engine/Dev/` — custom dev nodes (particles, 3D system, deep, Cycles)
- `Engine/Dev/Particles/` — particle simulation + rendering
- `Engine/Dev/Scene3D/` — 3D geometry, ScanlineRender, Card3D, etc.
- `Engine/Dev/Cycles/` — Cycles path tracer integration
- `Engine/Dev/Deep/` — deep compositing nodes
- `Gui/` — Qt6 user interface, viewport

## Key Documentation

- `Engine/Dev/WIKI.md` — particle system reference (all nodes, knobs, attributes, examples)
- `Engine/Dev/TODO.md` — known issues + user TODO list at the top
- `NODE_REGISTRY.md` — list of all registered plugin nodes
- `GIT_WORKFLOW.md` — full git rules (commit grouping, branch policy, what NOT to do)

## Particle System Status (2026-04-08)

Production-ready. All work documented in `Engine/Dev/WIKI.md`. Solver architecture is stateless forces + ParticleSolver runs the integrated loop. Renders in both ScanlineRender (4x MSAA, multi-sample motion blur) and CyclesRender (native instancing + motion blur).

## When in Doubt

- Read `Engine/Dev/TODO.md` to see what's planned vs done
- Read `Engine/Dev/WIKI.md` for particle system specifics
- Check `GIT_WORKFLOW.md` before any git operations
- Ask the user before making big architectural changes
