#!/usr/bin/env bash
# ==============================================================================
# NxNative Launcher - Safe Upstream Synchronization Script (seedds/nxnative)
# ==============================================================================
set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

echo "🔄 [NxNative Launcher] Checking upstream synchronization (seedds/nxnative)..."

# 1. Ensure upstream remote is configured
if ! git remote get-url upstream >/dev/null 2>&1; then
    echo "⚙️ Adding upstream remote (https://github.com/seedds/nxnative.git)..."
    git remote add upstream https://github.com/seedds/nxnative.git
fi

# 2. Fetch upstream updates
echo "📥 Fetching commits from upstream/main..."
git fetch upstream main

# 3. Verify local branch status
CURRENT_BRANCH=$(git rev-parse --abbrev-ref HEAD)
if [ "$CURRENT_BRANCH" != "main" ]; then
    echo "⚠️ Warning: You are currently on branch '$CURRENT_BRANCH'. Switching to 'main'..."
    git checkout main
fi

# 4. Check for unstaged or uncommitted changes
if ! git diff-index --quiet HEAD --; then
    echo "⚠️ Uncommitted modifications found in working directory:"
    git status --short
    echo ""
    echo "💡 Tip: Commit or run 'git stash' before syncing to prevent local conflicts."
    read -p "Temporarily stash working changes and proceed? (y/N): " -n 1 -r
    echo ""
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        git stash push -m "Auto-stash before sync-upstream on $(date)"
        STASHED=1
    else
        echo "❌ Synchronization aborted to protect uncommitted changes."
        exit 1
    fi
fi

# 5. Compare commit counts
BEHIND_COUNT=$(git rev-list --count HEAD..upstream/main)
if [ "$BEHIND_COUNT" -eq 0 ]; then
    echo "✅ Your fork is already 100% up to date with seedds/nxnative:main! No action required."
    if [ "${STASHED:-0}" -eq 1 ]; then
        echo "Restoring local stashed changes..."
        git stash pop
    fi
    exit 0
fi

echo "🚀 Found $BEHIND_COUNT new commits in upstream."
echo "--- Latest 5 commits in upstream ---"
git log HEAD..upstream/main --oneline -n 5
echo "------------------------------------"

# 6. Execute safe merge
echo "🔀 Merging upstream/main into your main branch..."
if git merge upstream/main --no-edit -m "chore(sync): merge upstream updates from seedds/nxnative"; then
    echo ""
    echo "🎉 Merge completed successfully with zero conflicts!"
    echo "All NxNative Launcher customizations have been preserved."
    echo ""
    echo "🎮 Regenerating SUPPORTED_GAMES.md from upstream game definitions..."
    python3 "$REPO_ROOT/scripts/generate_supported_games.py" 2>/dev/null || python3 "$REPO_ROOT/nxnative/scripts/generate_supported_games.py" 2>/dev/null || true
    if git status --porcelain | grep -q "SUPPORTED_GAMES.md"; then
        echo "📝 Committing updated SUPPORTED_GAMES.md..."
        git add SUPPORTED_GAMES.md nxnative/SUPPORTED_GAMES.md 2>/dev/null || true
        git commit -m "docs: auto-update SUPPORTED_GAMES.md from upstream game list" || true
    fi
    echo ""
    echo "👉 To publish the updates to your GitHub fork, run:"
    echo "   git push origin main"
else
    echo ""
    echo "⚠️ MERGE CONFLICT DETECTED!"
    echo "Aborting merge to safeguard your local changes..."
    git merge --abort
    echo "Your main branch has been restored to its intact state."
    echo "To inspect and resolve conflicts manually, run:"
    echo "   git merge upstream/main"
fi

if [ "${STASHED:-0}" -eq 1 ]; then
    echo ""
    echo "Restoring local stashed changes..."
    git stash pop
fi
