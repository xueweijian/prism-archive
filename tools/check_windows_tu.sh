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
#   * the gate is -fsyntax-only: it produces no artifacts and runs in ~2s;
#   * the Windows CI lane remains the only proof that the TUs *build and work*.
#
# Usage: sh tools/check_windows_tu.sh
set -eu

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail=0
# Each probe includes ONE Windows header, so a name that only compiles because
# another header pulled something in transitively fails here (CONTRACTS B2).
cat > "$tmp/vdragdrop.cpp" <<'EOF'
#include <cstdint>
#include <string>
#include "PrismVirtualDragDrop.hpp"

int main() { return 0; }
EOF

cat > "$tmp/dragbridge.cpp" <<'EOF'
#include <cstdint>
#include <string>
#include "PrismDragDecisionBridge.hpp"

int main() { return 0; }
EOF

# The headers spell their SDK includes as <windows.h>, <wrl/client.h>, ... so
# the stub directory has to answer those exact names.
mkdir -p "$tmp/inc"
cp tools/win_stub/win_stub.hpp "$tmp/inc/windows.h"
cp tools/win_stub/win_stub.hpp "$tmp/inc/shlobj.h"
cp tools/win_stub/win_stub.hpp "$tmp/inc/shlwapi.h"
cp tools/win_stub/win_stub.hpp "$tmp/inc/exdisp.h"
cp tools/win_stub/win_stub.hpp "$tmp/inc/exdisp2.h"
cp tools/win_stub/win_stub.hpp "$tmp/inc/propidl.h"
cp tools/win_stub/win_stub.hpp "$tmp/inc/wincodec.h"
cp tools/win_stub/win_stub.hpp "$tmp/inc/objbase.h"
mkdir -p "$tmp/inc/wrl"
cp tools/win_stub/wrl_stub.hpp "$tmp/inc/wrl/client.h"
cp tools/win_stub/wrl_stub.hpp "$tmp/inc/wrl/implements.h"

# Src/App/Main.cpp is deliberately NOT here. It would need a D2D/DWrite
# vocabulary to reach, and a half-invented COM surface is a gate that can lie --
# which is exactly what this file's contract forbids. Its compile coverage is the
# Windows lane (m2/m3 compile-check TUs), and the THREADING invariant that batch
# A violated is locked instead by preflight's static B9 rule, which needs no SDK
# vocabulary at all. Do not "fix" this by growing the stub.

for probe in vdragdrop dragbridge; do
  # -Werror matches the project's PRISM_WARNINGS_AS_ERRORS=ON default.
  if ! out=$(g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror \
                  -fsyntax-only \
                  -isystem "$tmp/inc" -I tools/win_stub/engine -I Src/PrismWin -I Src/PrismCore \
                  "$tmp/$probe.cpp" 2>&1); then
    fail=1
    echo "FAIL[windows-tu]: $probe.cpp"
    echo "$out" | head -20 | sed 's/^/    /'
  fi
done

if [ "$fail" -ne 0 ]; then
  echo "WINDOWS-TU GATE: FAILED"
  echo "  (the stub is vocabulary only -- fix the real header, not the stub)"
  exit 1
fi
echo "WINDOWS-TU GATE: PASSED (2 headers typecheck standalone under -Wall -Wextra -Wpedantic -Werror)"
