# Git Workflow

Standard practices for committing and pushing changes to this Natron fork.

## Repo Structure

- **Remote:** `https://github.com/Niik-l/Natron` (personal fork of NatronGitHub)
- **Main branch:** `RB-2.6` (release branch 2.6, based on upstream)
- **Build dir:** `build-qt6/` (gitignored — never commit build artifacts)

## Golden Rules

1. **Feature branches for risky / experimental work.** Multi-week refactors, GPU ports, anything you might throw away → cut a `feature/*` or `experiment/*` branch first. Small verified fixes and incremental features can go directly to `RB-2.6` — this is a solo fork, not a team repo, and ceremony for ceremony's sake is just noise. If/when collaborators join, default back to feature branches for everything.
2. **Never rewrite published history.** No `git rebase -i` or `git push --force` on commits that have been pushed.
3. **Never commit build artifacts.** Anything in `build-qt6/`, `*.obj`, `*.dll`, `*.exe`, etc.
4. **Logical commits.** One commit = one logical change. Don't lump unrelated work together.
5. **Always test before pushing.** Build and run the changes to confirm they work.

## Standard Feature Branch Workflow

For any new feature, fix, or experiment:

```bash
# 1. Make sure you're up to date on RB-2.6
git checkout RB-2.6
git pull origin RB-2.6

# 2. Create a feature branch
git checkout -b feature/short-description
# Examples: feature/particle-instancing, fix/cycles-motion-blur, docs/wiki-update

# 3. Make your changes, testing as you go
# ... edit files, build, test ...

# 4. Stage and commit in logical chunks
git status
git add Engine/Dev/Particles/...
git commit -m "Particles: add ParticleInstance node + Cycles instancing"

# 5. Push the feature branch to origin
git push -u origin feature/short-description

# 6. Merge to RB-2.6 when ready
git checkout RB-2.6
git merge feature/short-description
git push origin RB-2.6

# 7. Optionally delete the merged branch
git branch -d feature/short-description
git push origin --delete feature/short-description
```

## Branch Naming Convention

| Prefix | Use for | Example |
|--------|---------|---------|
| `feature/` | New features or nodes | `feature/particle-system-overhaul` |
| `fix/` | Bug fixes | `fix/cycles-shader-leak` |
| `docs/` | Documentation only | `docs/particle-wiki` |
| `refactor/` | Code refactoring with no behavior change | `refactor/scenegraph-cleanup` |
| `experiment/` | Experimental work, may be discarded | `experiment/gpu-particles` |

Use kebab-case (`particle-system-overhaul`, not `ParticleSystemOverhaul`).

## Commit Message Format

```
<area>: <short summary>

<optional longer description with bullet points>
- What changed
- Why it changed
- Any follow-ups needed
```

Examples:
- `Particles: add ParticleInstance node + Cycles native instancing`
- `Cycles: fix motion blur on instanced geo (was using wrong API)`
- `Docs: add particle WIKI.md with all node knobs`

Keep the first line under ~70 characters.

## What NOT to commit

- Build artifacts: `build-qt6/`, `*.obj`, `*.dll`, `*.exe`, `moc_*.cpp`
- Editor files: `.vscode/`, `.idea/`, `*.swp`
- OS junk: `.DS_Store`, `Thumbs.db`
- Personal scratch: `tools/abc_dump.cpp` (debug-only tools — keep local)
- Secrets: API keys, passwords, tokens (Natron has none, but the rule stands)

If `git status` shows hundreds of files, something is wrong with `.gitignore`. Stop and check.

## Clean Up Before Committing

```bash
# See what's actually changed (excluding build artifacts)
git status --short | grep -v "build-qt6/"

# See the actual diff before committing
git diff Engine/Dev/Particles/ParticleEmitter.cpp

# Stage selectively
git add -p  # interactive staging by hunk
```

## When Things Go Wrong

| Problem | Solution |
|---------|----------|
| Committed to wrong branch | `git reset HEAD~1`, switch branch, re-commit |
| Want to undo last commit (not pushed) | `git reset --soft HEAD~1` (keeps changes) |
| Want to discard local changes to a file | `git checkout -- path/to/file` |
| Accidentally added a build file | `git rm --cached path/to/file` |
| Want to see what's in a commit | `git show <commit-hash>` |
| Want to undo a pushed commit | `git revert <commit-hash>` (creates a new "undo" commit) |

**Rule:** Never use `git push --force` on a shared branch. If you absolutely must, use `--force-with-lease` which is safer.

## Why This Matters

- **Reviewable history** — anyone (including future you) can see "this branch contained the particle work" instead of "everything is in one giant commit"
- **Safe experiments** — bad branch? Delete it. No damage to main.
- **Easy rollbacks** — revert a feature without losing other work
- **Industry standard** — every studio (ILM, Weta, Framestore, MPC, etc.) and every major open source project uses this pattern
- **Future-proof** — if anyone else ever joins this project, they expect this workflow

## See Also

- GitHub Flow: https://docs.github.com/en/get-started/using-github/github-flow
- Atlassian Feature Branch Workflow: https://www.atlassian.com/git/tutorials/comparing-workflows/feature-branch-workflow
- Pro Git book: https://git-scm.com/book/en/v2
