#!/usr/bin/env bash
# Refactor guardrail: baseline code metrics for SaidaEngine.
#
# Prints first-party LOC, oversized files, per-directory weight and a
# long-function heuristic so each refactor (see ROADMAP.md §2) can be
# measured objectively. Read-only; run from the repo root:
#
#   tools/code_metrics.sh
#
set -euo pipefail

ROOTS="src web tools tests WitnessGame"
EXCLUDE='third_party|/build/|_deps|node_modules'
FILE_MAX=600   # target: no first-party file above this
FUNC_MAX=80    # target: no function above this

srcfiles() {
  find $ROOTS -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.cc' \) 2>/dev/null \
    | grep -viE "$EXCLUDE"
}

echo "=== SaidaEngine code metrics ($(date +%Y-%m-%d)) ==="
total=$(srcfiles | xargs wc -l 2>/dev/null | tail -1 | awk '{print $1}')
count=$(srcfiles | wc -l)
printf "first-party C/C++ files : %s\n" "$count"
printf "first-party LOC         : %s\n\n" "$total"

echo "--- files over ${FILE_MAX} lines (target: 0) ---"
srcfiles | xargs wc -l 2>/dev/null \
  | awk -v m=$FILE_MAX 'NF==2 && $2!="total" && $1>m {printf "%6d  %s\n",$1,$2}' \
  | sort -rn
echo

echo "--- LOC per src/ subdir ---"
for d in $(ls -d src/*/ 2>/dev/null); do
  files=$(find "$d" -type f \( -name '*.cpp' -o -name '*.hpp' \) 2>/dev/null | grep -viE "$EXCLUDE")
  n=$(printf '%s\n' "$files" | grep -c . || true)
  loc=$(printf '%s\n' "$files" | xargs cat 2>/dev/null | wc -l)
  printf "%-26s %3d files %7d loc\n" "$d" "$n" "$loc"
done | sort -k4 -rn
echo

echo "--- long-function heuristic: top-level def gaps > ${FUNC_MAX} lines ---"
echo "(approximate: distance between consecutive Type::method( definitions)"
while read -r f; do
  { grep -nE '^[A-Za-z_].*::[A-Za-z_~]+\(' "$f" 2>/dev/null | cut -d: -f1 \
      | awk -v file="$f" -v m=$FUNC_MAX 'NR>1 && ($1-prev)>m {printf "%5d lines  %s:%d\n",($1-prev),file,prev} {prev=$1}'; } || true
done < <(srcfiles | grep '\.cpp$') | sort -rn | head -20
