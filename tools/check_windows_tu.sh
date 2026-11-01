#!/bin/sh
# Windows-TU typecheck gate (L4: environment differences, caught locally).
#
# The Windows layer cannot be BUILT here (no MSVC, no 7z.dll, no Shell), but
# roughly half this project's historical CI failures were not behavioural --
# they were compile errors: a wrong override signature, an unparenthesized
# std::min under the windows.h macro, a member used before it was declared, a
# mismatched std::chrono template argument, a std::function built from a
# nullptr literal. All of those are detectable by a C++ front end given the SDK
# *vocabulary*, which is all tools/win_stub provides.
#
# Discipline (this is the part that keeps the gate honest):
#   * the stub declares types, macros and vtable SHAPES only;
#   * it never implements 7z.dll, the Shell, or the stream under test, so no
#     behavioural claim can leak out of this gate;
#   * the gate is -fsyntax-only: it produces no artifacts;
#   * the Windows CI lane remains the only proof that the TUs *build and work*.
#
# Portability of the GATE itself (2026-11-01, card C2):
#   * CXX-overridable like check_headers.sh (default g++);
#   * it anchors its own working directory at the repo root, so it can be driven
#     from anywhere (tools/local_gates.ps1 calls it from Windows through WSL);
#   * the stub directory is checked for explicitly: when it was missing, this
#     script's first cp failed with a bare "No such file or directory" and the
#     gate looked broken rather than mis-provisioned (that is why nothing noticed
#     the stub had never been pushed -- CONTRACTS D1);
#   * the probes compile in parallel (2 of them, both independent TUs).
#
# Usage: sh tools/check_windows_tu.sh
# Env:   CXX=<compiler>
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

CXX=${CXX:-g++}
if ! command -v "$CXX" >/dev/null 2>&1; then
  echo "WINDOWS-TU GATE: FAILED (CXX='$CXX' is not on PATH)" >&2
  echo "  set CXX to a C++20 compiler, e.g. CXX=g++-13 sh tools/check_windows_tu.sh" >&2
  exit 2
fi

if [ ! -f tools/win_stub/win_stub.hpp ] || [ ! -f tools/win_stub/wrl_stub.hpp ]; then
  echo "WINDOWS-TU GATE: FAILED (tools/win_stub/ is missing -- the SDK vocabulary)"
  echo "  the stub is part of the tree on purpose: a gate whose inputs are not"
  echo "  committed can only ever fail (CONTRACTS D1)."
  exit 2
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
start=$(date +%s)

# Each probe includes ONE Windows header, so a name that only compiles because
# another header pulled something in transitively fails here (CONTRACTS B2).
emit_probe() {
  # $1 = probe basename, $2 = header to include
  cat > "$tmp/$1.cpp" <<EOF
#include <cstdint>
#include <string>
#include "$2"

int main() { return 0; }
EOF
}

emit_probe vdragdrop   PrismVirtualDragDrop.hpp
emit_probe dragbridge  PrismDragDecisionBridge.hpp

# The headers spell their SDK includes as <windows.h>, <wrl/client.h>, ... so
# the stub directory has to answer those exact names.
mkdir -p "$tmp/inc"
for hdr in windows.h shlobj.h shlwapi.h exdisp.h exdisp2.h propidl.h \
           wincodec.h objbase.h; do
  cp tools/win_stub/win_stub.hpp "$tmp/inc/$hdr"
done
mkdir -p "$tmp/inc/wrl"
cp tools/win_stub/wrl_stub.hpp "$tmp/inc/wrl/client.h"
cp tools/win_stub/wrl_stub.hpp "$tmp/inc/wrl/implements.h"

# Src/App/Main.cpp is deliberately NOT here. It would need a D2D/DWrite
# vocabulary to reach, and a half-invented COM surface is a gate that can lie --
# which is exactly what this file's contract forbids. Its compile coverage is the
# Windows lane (m2/m3 compile-check TUs), and the THREADING invariant that batch
# A violated is locked instead by preflight's static B9 rule, which needs no SDK
# vocabulary at all. Do not "fix" this by growing the stub.

typecheck() {
  # $1 = probe basename; writes $tmp/$1.log and $tmp/$1.rc
  _probe=$1
  # -Werror matches the project's PRISM_WARNINGS_AS_ERRORS=ON default.
  if "$CXX" -std=c++20 -Wall -Wextra -Wpedantic -Werror \
        -fsyntax-only \
        -isystem "$tmp/inc" -I tools/win_stub/engine -I Src/PrismWin -I Src/PrismCore \
        "$tmp/$_probe.cpp" > "$tmp/$_probe.log" 2>&1; then
    printf '0\n' > "$tmp/$_probe.rc"
  else
    printf '1\n' > "$tmp/$_probe.rc"
  fi
}

probes="vdragdrop dragbridge"
pids=""
for probe in $probes; do
  typecheck "$probe" &
  pids="$pids $!"
done
for p in $pids; do
  wait "$p" || true
done

fail=0
count=0
for probe in $probes; do
  count=$((count + 1))
  if [ "$(cat "$tmp/$probe.rc" 2>/dev/null || echo 1)" != "0" ]; then
    fail=1
    echo "FAIL[windows-tu]: $probe.cpp"
    head -20 "$tmp/$probe.log" | sed 's/^/    /'
  fi
done

elapsed=$(( $(date +%s) - start ))
if [ "$fail" -ne 0 ]; then
  echo "WINDOWS-TU GATE: FAILED"
  echo "  (the stub is vocabulary only -- fix the real header, not the stub)"
  exit 1
fi
ver=$("$CXX" --version 2>/dev/null | head -1)
printf 'WINDOWS-TU GATE: PASSED (%s headers typecheck standalone under -Wall -Wextra -Wpedantic -Werror, %ss, %s)\n' \
    "$count" "$elapsed" "$ver"
