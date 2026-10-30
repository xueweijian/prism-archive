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
# on which libstdc++ happens to be installed.
#
# Usage: tools/check_headers.sh [include-dir ...]   (defaults to Src/PrismCore)
set -eu

dirs="${*:-Src/PrismCore}"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0
count=0

for dir in $dirs; do
  for header in "$dir"/*.hpp; do
    [ -e "$header" ] || continue
    name=$(basename "$header")
    count=$((count + 1))

    # ---- LEVEL 1: standalone compile -------------------------------------
    printf '#include "%s"\nint main() { return 0; }\n' "$name" > "$tmp/probe.cpp"
    if ! out=$(g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -I "$dir" \
                    -fsyntax-only "$tmp/probe.cpp" 2>&1); then
      fail=1
      echo "FAIL[standalone]: $header"
      echo "$out" | head -5 | sed 's/^/    /'
    fi

    # ---- LEVEL 2: fixed-width integer types need <cstdint> itself --------
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

if [ "$fail" -ne 0 ]; then
  echo "HEADER GATE: FAILED"
  exit 1
fi
echo "HEADER GATE: PASSED ($count headers: standalone + include hygiene)"
