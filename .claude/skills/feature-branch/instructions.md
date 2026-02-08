# Feature Branch Workflow Skill

Multi-agent development workflow for implementing features on isolated branches.

## Usage

```
/feature-branch <branch-name> <task-description>
```

## Workflow

### 1. Create Feature Branch

```bash
# Ensure main is up-to-date
git checkout main
git pull origin main

# Create and switch to feature branch
git checkout -b feature/<name>
```

### 2. Implement Feature

Work on the feature with regular commits:

```bash
# Stage specific files (not git add -A)
git add <file1> <file2>

# Commit with descriptive message
git commit -m "$(cat <<'EOF'
<type>: <subject>

<body explaining what and why>

Co-Authored-By: Claude <noreply@anthropic.com>
EOF
)"
```

**Commit types:** feat, fix, refactor, docs, test, chore

### 3. Push Feature Branch

```bash
git push -u origin feature/<name>
```

### 4. Create Pull Request

```bash
gh pr create --title "<type>: <subject>" --body "$(cat <<'EOF'
## Summary
- <bullet points>

## Test plan
- [ ] Unit tests pass
- [ ] Integration tests pass
- [ ] Manual testing done

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

### 5. After Review/Merge

```bash
# Switch back to main
git checkout main
git pull origin main

# Delete local feature branch
git branch -d feature/<name>
```

## Multi-Agent Coordination

When multiple agents work on different features:

1. **Each agent gets its own branch** - No conflicts between parallel work
2. **Regular pushes** - Push after each logical unit of work
3. **Small PRs** - Easier to review, less merge conflict risk
4. **Clear naming** - `feature/<module>-<feature>` (e.g., `feature/ralph-api`)

## Branch Naming Conventions

| Type | Pattern | Example |
|------|---------|---------|
| Feature | `feature/<name>` | `feature/ralph-api` |
| Bugfix | `fix/<issue>` | `fix/lu-pivot-threshold` |
| Refactor | `refactor/<name>` | `refactor/simplex-cleanup` |
| Docs | `docs/<name>` | `docs/api-annotations` |

## Example: Ralph API Implementation

```bash
# Create branch
git checkout -b feature/ralph-api

# Phase 1: Core handler
# ... implement ralph_api.h, ralph_api.c, ralph_parse_lp.c ...
git add ralph/include/ralph_api.h ralph/src/ralph_api.c ralph/src/ralph_parse_lp.c
git commit -m "feat(ralph): add transport-agnostic API handler"

# Phase 2: HTTP server
# ... implement api/src/main.c ...
git add ralph/api/
git commit -m "feat(ralph): add Mongoose HTTP wrapper"

# Phase 3: WASM
# ... implement wasm/ ...
git add ralph/wasm/
git commit -m "feat(ralph): add WASM build"

# Push and create PR
git push -u origin feature/ralph-api
gh pr create --title "feat(ralph): REST API and WASM demo" --body "..."
```

## Conflict Resolution

If main has diverged:

```bash
# Fetch latest
git fetch origin main

# Rebase onto main (preferred for clean history)
git rebase origin/main

# Or merge (if rebase is complex)
git merge origin/main

# Push (force if rebased)
git push --force-with-lease  # Safe force push
```

## Checklist Before PR

- [ ] All tests pass (`make test`)
- [ ] Code follows naming conventions
- [ ] No debug/temp code left
- [ ] Commits are logical units
- [ ] PR description is complete
