#!/bin/sh
# Header self-containment gate.
#
# Two levels, because a single "does it compile alone?" probe is NOT enough --
# the local libstdc++ incidentally pulls in things CI's build does not, which is
# exactly how PasswordVault.hpp shipped a uint8_t without <cstdint> and only the
# GitHub runner noticed.
#
#  LEVEL 1  standalone compile: each header compiles alone under -Werror.
#  LEVEL 2  include hygiene: if a header USES a fixed-width integer type
#           (uint8_t / uint32_t / int64_t / size_t-as-cstddef...), it must
#           include <cstdint> ITSELF, never rely on a transitive pull.
#
# Level 2 is a lexical check on purpose: it is deterministic and does not depend
# on which libstdc++ happens to be installed (CONTRACTS B2/B3).
#
# Portability and cost of the GATE itself (2026-11-01, card C2):
#   * the compiler is CXX-overridable -- CI uses g++, a Windows dev box runs this
#     through WSL (tools/local_gates.ps1), a clang box sets CXX=clang++;
#   * the script anchors its own working directory at the repo root, so it can be
#     invoked from anywhere;
#   * LEVEL 1 compiles run JOBS-way parallel (default: nproc). 21 sequential
#     compiles were 205s on this project's 4-core dev box -- a gate that
#     expensive gets skipped, and a skipped gate is no gate. The transcript stays
#     in header order (each failure prints its own log head), so parallelism
#     costs nothing in readability;
#   * a missing compiler is a LOUD failure (exit 2), never a silent skip
#     (CONTRACTS D1).
#
# Usage: tools/check_headers.sh [include-dir ...]   (defaults to Src/PrismCore)
# Env:   CXX=<compiler>   JOBS=<n>
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

CXX=${CXX:-g++}
if ! command -v "$CXX" >/dev/null 2>&1; then
  echo "HEADER GATE: FAILED (CXX='$CXX' is not on PATH)" >&2
  echo "  set CXX to a C++20 compiler, e.g. CXX=g++-13 sh tools/check_headers.sh" >&2
  exit 2
fi

if [ "$#" -gt 0 ]; then
  dirs="$*"
else
  dirs="Src/PrismCore"
fi

JOBS=${JOBS:-}
if [ -z "$JOBS" ]; then
  JOBS=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
fi
case $JOBS in
  ''|*[!0-9]*) JOBS=1 ;;
esac
[ "$JOBS" -ge 1 ] || JOBS=1

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0
count=0
start=$(date +%s)

# ---- LEVEL 2 (lexical, sequential, cheap) --------------------------------
# Runs first so a missing <cstdint> is reported before any compile noise.
for dir in $dirs; do
  for header in "$dir"/*.hpp; do
    [ -e "$header" ] || continue
    if grep -qE '\b(u?int(8|16|32|64)_t)\b' "$header"; then
      if ! grep -q '#include <cstdint>' "$header"; then
        fail=1
        echo "FAIL[cstdint]: $header uses a fixed-width integer type but does"
        echo "    not include <cstdint> itself (relies on a transitive include"
        echo "    that differs between libstdc++ versions and CI)."
      fi
    fi
  done
done

# ---- LEVEL 1 (parallel standalone compiles) ------------------------------
: > "$tmp/list"
for dir in $dirs; do
  for header in "$dir"/*.hpp; do
    [ -e "$header" ] || continue
    count=$((count + 1))
    printf '%s|%s\n' "$dir" "$header" >> "$tmp/list"
  done
done

compile_one() {
  # $1 = include dir, $2 = header, $3 = log path, $4 = unique id
  _dir=$1
  _header=$2
  _log=$3
  _id=$4
  _name=$(basename "$_header")
  printf '#include "%s"\nint main() { return 0; }\n' "$_name" > "$tmp/probe.$_id.cpp"
  if "$CXX" -std=c++20 -Wall -Wextra -Wpedantic -Werror -I "$_dir" \
         -fsyntax-only "$tmp/probe.$_id.cpp" > "$_log" 2>&1; then
    printf '0\n' > "$_log.rc"
  else
    printf '1\n' > "$_log.rc"
  fi
}

i=0
while [ "$i" -lt "$count" ]; do
  pids=""
  launched=0
  while [ "$launched" -lt "$JOBS" ] && [ "$i" -lt "$count" ]; do
    line=$(sed -n "$((i + 1))p" "$tmp/list")
    hdir=${line%%|*}
    hfile=${line#*|}
    id=$((i + 1))
    compile_one "$hdir" "$hfile" "$tmp/out.$id" "$id" &
    pids="$pids $!"
    i=$((i + 1))
    launched=$((launched + 1))
  done
  for p in $pids; do
    wait "$p" || true
  done
done

# Report in header order: parallel execution, deterministic transcript.
id=0
while [ "$id" -lt "$count" ]; do
  id=$((id + 1))
  if [ -f "$tmp/out.$id.rc" ] && [ "$(cat "$tmp/out.$id.rc")" != "0" ]; then
    # NOTE: the "p" must stay outside the quotes -- "sed -n $id file" is an
    # address with no command and dies with "missing command", i.e. a failing
    # header would kill the gate instead of reporting it (CONTRACTS D3).
    line=$(sed -n "$id"p "$tmp/list")
    fail=1
    echo "FAIL[standalone]: ${line#*|}"
    head -5 "$tmp/out.$id" | sed 's/^/    /'
  fi
done

elapsed=$(( $(date +%s) - start ))
if [ "$fail" -ne 0 ]; then
  echo "HEADER GATE: FAILED"
  exit 1
fi
ver=$("$CXX" --version 2>/dev/null | head -1)
printf 'HEADER GATE: PASSED (%s headers: standalone + include hygiene, -j%s, %ss, %s)\n' \
    "$count" "$JOBS" "$elapsed" "$ver"
