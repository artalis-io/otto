---
name: agent-worktree
description: Create isolated git worktree for parallel agent development. Use when spawning agents that work on the same codebase without collisions.
user-invocable: true
---

# Agent Worktree Skill

Create an isolated git worktree for parallel agent development. Each agent gets its own working directory with its own feature branch - no branch switching conflicts.

**Arguments:** $ARGUMENTS

## Usage

```
/agent-worktree <task-description>
```

**Example:**
```
/agent-worktree Add LRU caching to Velo router
```

## How It Works

```
otto/                      # Main worktree (main branch, user's primary)
otto-agent-a1b2c3/         # Agent 1's isolated workspace (feature/a1b2c3-velo-cache)
otto-agent-d4e5f6/         # Agent 2's isolated workspace (feature/d4e5f6-ralph-api)
```

- Each agent works in its own directory with its own branch
- No `git checkout` conflicts - worktrees are locked to their branches
- Shared `.git` database - commits are immediately visible after push
- Main worktree (`otto/`) stays on `main` branch, never touched by agents

## Workflow

### 1. Generate Unique ID

Generate a short unique ID for this agent session:

```bash
AGENT_ID=$(head -c 4 /dev/urandom | xxd -p)
echo "Agent ID: $AGENT_ID"
```

### 2. Ensure Main Is Up-to-Date

From the main worktree, sync with remote:

```bash
cd /path/to/otto
git fetch origin
git pull origin main
```

### 3. Create Worktree with Feature Branch

Create an isolated worktree as a sibling directory:

```bash
# From main worktree
BRANCH_NAME="feature/${AGENT_ID}-<short-task-name>"
WORKTREE_PATH="../otto-agent-${AGENT_ID}"

git worktree add "$WORKTREE_PATH" -b "$BRANCH_NAME"
```

**Branch naming:**
- Format: `feature/<agent-id>-<module>-<feature>`
- Examples:
  - `feature/a1b2c3d4-velo-cache-lru`
  - `feature/e5f6a7b8-ralph-api-mps`
  - `fix/c9d0e1f2-carta-clip-bug`

### 4. Work in the Worktree

All work happens in the agent's worktree:

```bash
cd "$WORKTREE_PATH"

# Implement with regular commits
git add <files>
git commit -m "$(cat <<'EOF'
<type>(<scope>): <subject>

<body>
EOF
)"
```

**Commit types:** `feat`, `fix`, `refactor`, `docs`, `test`, `chore`

**Attribution:** do not add `Co-Authored-By: Claude`, `Claude-Session:`, or a
"Generated with Claude Code" line to commits or PR descriptions in this repo.
`.claude/settings.json` turns the automatic trailers off; leave them off when
writing a message by hand too.

### 5. Sync with Main (If Needed)

If main has progressed while working:

```bash
cd "$WORKTREE_PATH"
git fetch origin main
git rebase origin/main
```

### 6. Push and Create PR

```bash
git push -u origin "$BRANCH_NAME"

gh pr create --title "<type>(<scope>): <subject>" --body "$(cat <<'EOF'
## Summary
- <bullet points>

## Test plan
- [ ] `make test` passes
- [ ] Manual verification done
EOF
)"
```

### 7. Cleanup (After PR Merged)

After the PR is merged, clean up the worktree:

```bash
# From main worktree (otto/)
cd /path/to/otto

# Remove the worktree
git worktree remove "../otto-agent-${AGENT_ID}"

# Delete the local branch (remote branch deleted by PR merge)
git branch -d "feature/${AGENT_ID}-<task-name>"

# Prune stale worktree references
git worktree prune
```

**Auto-cleanup:** Agents MUST clean up their worktree after PR is merged or work is abandoned.

## Multi-Agent Coordination

### Parallel Development Pattern

When spawning multiple agents with the Task tool:

```
Task 1: "Work in /path/to/otto-agent-a1b2c3 on adding Velo caching..."
Task 2: "Work in /path/to/otto-agent-d4e5f6 on Ralph API endpoints..."
```

Each agent:
1. Works only in its assigned worktree
2. Never touches other worktrees or main
3. Pushes regularly to its feature branch
4. Creates PR when done
5. Cleans up after merge

### Syncing Between Agents

If Agent 2 needs Agent 1's work:

```bash
# Agent 2's worktree
cd /path/to/otto-agent-d4e5f6

# After Agent 1's PR is merged to main
git fetch origin main
git rebase origin/main
```

### Viewing All Worktrees

```bash
# From any worktree
git worktree list

# Or list sibling directories
ls -la ../otto-agent-*
```

## Session Handoff

If a session ends mid-work, persist state to the module's roadmap:

```markdown
## WIP: <Feature Name> (Session Handoff)

**Status:** In progress, paused at <phase>
**Worktree:** ../otto-agent-<id>
**Branch:** feature/<id>-<task-name>
**Last commit:** <hash>
**Next steps:**
1. ...
2. ...
```

The next session can:
1. Read the roadmap to find the worktree
2. `cd` to the worktree and continue
3. Delete the WIP section when work completes

## Checklist

Before creating PR:
- [ ] All tests pass (`make test`)
- [ ] Code follows naming conventions
- [ ] No debug/temp code left
- [ ] Commits are logical units

After PR merged:
- [ ] Worktree removed (`git worktree remove`)
- [ ] Local branch deleted (`git branch -d`)
- [ ] WIP section removed from roadmap (if any)

## Quick Reference

| Action | Command |
|--------|---------|
| Create worktree | `git worktree add ../otto-agent-ID -b feature/ID-name` |
| List worktrees | `git worktree list` |
| Remove worktree | `git worktree remove ../otto-agent-ID` |
| Prune stale | `git worktree prune` |
| Sync with main | `git fetch origin main && git rebase origin/main` |

## Example: Complete Flow

```bash
# 1. Generate ID and create worktree
AGENT_ID=$(head -c 4 /dev/urandom | xxd -p)
cd /Users/mark/Desktop/work/artalis-io/otto
git fetch origin && git pull origin main
git worktree add "../otto-agent-${AGENT_ID}" -b "feature/${AGENT_ID}-velo-cache"

# 2. Work in worktree
cd "../otto-agent-${AGENT_ID}"
# ... implement feature ...
git add velo/src/vl_cache.c velo/include/vl_cache.h
git commit -m "feat(velo): add LRU route caching"

# 3. Push and create PR
git push -u origin "feature/${AGENT_ID}-velo-cache"
gh pr create --title "feat(velo): add LRU route caching" --body "..."

# 4. After merge, cleanup
cd /Users/mark/Desktop/work/artalis-io/otto
git worktree remove "../otto-agent-${AGENT_ID}"
git branch -d "feature/${AGENT_ID}-velo-cache"
```
