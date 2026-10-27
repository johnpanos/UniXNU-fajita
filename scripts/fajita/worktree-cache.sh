#!/bin/bash
# scripts/fajita/worktree-cache.sh [MAIN_TREE]
#
# Give a new board worktree the shared inputs of the main tree (AGENTS.md, "One tree"): .cache/<entry> -> MAIN_TREE/.cache/<entry>
# for every entry except the build outputs (fajita-out*, which stay per worktree), and ios16/.cache -> the main tree's ios16/.cache
# (firmware, IPSW). build.lock and fajita-work are shared on purpose: one build at a time, one phone lock. Run it once in the new
# worktree; it never replaces an existing entry. MAIN_TREE defaults to ../UniXNU-fajita-ios16 next to this worktree.
set -eo pipefail
BOARD=$(cd "$(dirname "$0")/../.." && pwd)
MAIN=$(cd "${1:-$BOARD/../UniXNU-fajita-ios16}" && pwd)
[ "$MAIN" != "$BOARD" ] || { echo "this is the main tree"; exit 1; }
[ -d "$MAIN/.cache" ] || { echo "no $MAIN/.cache"; exit 1; }
mkdir -p "$BOARD/.cache"
n=0
for e in "$MAIN"/.cache/*; do
  b=$(basename "$e")
  case $b in fajita-out*) continue ;; esac
  [ -e "$BOARD/.cache/$b" ] || [ -L "$BOARD/.cache/$b" ] || { ln -s "$e" "$BOARD/.cache/$b"; n=$((n + 1)); }
done
[ -e "$BOARD/ios16/.cache" ] || [ -L "$BOARD/ios16/.cache" ] || { ln -s "$MAIN/ios16/.cache" "$BOARD/ios16/.cache"; n=$((n + 1)); }
echo "linked $n entries from $MAIN"
