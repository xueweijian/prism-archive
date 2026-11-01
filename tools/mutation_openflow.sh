#!/bin/sh
# Negative-control harness for OpenFlow / Probe (CONTRACTS D3 + D5).
#
# For each mutation: copy the workspace, apply the textual mutation, ASSERT THE
# SUBSTITUTION ACTUALLY MATCHED (a replacement string that misses is a silent
# no-op, and "the suite stayed green" would then mean nothing), rebuild, run,
# and require a RED suite. Then verify the pristine tree is green again.
set -u
SRC=/tmp/ws_a3/repo
WORK=/tmp/mut_openflow
FILE_REL=Src/PrismCore/OpenFlow.hpp
RING_REL=Src/PrismCore/PasswordRing.hpp

run_case() {
  NAME="$1"; FILE="$2"; OLD="$3"; NEW="$4"; NEEDLE="$5"
  rm -rf "$WORK"
  mkdir -p "$WORK"
  # Copy the tree WITHOUT .git and build/: PROot refuses to stat some pack
  # objects and a partial copy leaves the layout inconsistent for the rest of
  # the run (which then reports "injection did not match" for the wrong reason).
  tar -C "$SRC" --exclude=.git --exclude=build -cf - . 2>/dev/null | tar -C "$WORK" -xf - 2>/dev/null
  if [ ! -f "$WORK/Src/PrismCore/PasswordRing.hpp" ]; then
    echo "[$NAME] work-tree copy failed -- result void"; return 2
  fi
  python3 - "$WORK/$FILE" "$OLD" "$NEW" <<'PYEOF'
import sys
path, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
src = open(path, encoding='utf-8').read()
if src.count(old) != 1:
    print("INJECTION-FAILED count=%d" % src.count(old))
    sys.exit(3)
open(path, 'w', encoding='utf-8').write(src.replace(old, new))
print("INJECTION-APPLIED")
PYEOF
  if [ $? -ne 0 ]; then echo "[$NAME] INJECTION DID NOT MATCH -- result void"; return 2; fi
  # Independent proof the mutation is present in the built tree.
  grep -qF "$NEEDLE" "$WORK/$FILE" && echo "[$NAME] post-inject grep: FOUND" || {
    echo "[$NAME] post-inject grep: MISSING -- result void"; return 2; }

  cmake -S "$WORK" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release \
        -DPRISM_BUILD_WIN_LAYER=OFF >"$WORK/cfg.log" 2>&1
  if ! cmake --build "$WORK/build" --target test_open_flow -j4 >"$WORK/build.log" 2>&1; then
    echo "[$NAME] RED (compile failure) -- counts as caught, see build.log"; return 0
  fi
  OUT=$("$WORK/build/Tests/test_open_flow" 2>&1)
  echo "$OUT" | grep -E "FAILED|FAIL " | head -4
  if echo "$OUT" | grep -q "0 failures"; then
    echo "[$NAME] STILL GREEN -- mutation not caught  <<< BAD"
    return 1
  fi
  echo "[$NAME] RED as required: $(echo "$OUT" | grep -o '[0-9]* failures')"
  return 0
}

echo "================ MUTATION 1: drop the WrongPassword branch ============"
# ClassifyOpen currently distinguishes "locked and we already tried" from
# "locked and we have not". Flatten it: a wrong password becomes NeedPassword,
# i.e. the flow re-prompts forever instead of reporting a bad password.
run_case "m1-wrongpassword-becomes-needpassword" "$FILE_REL" \
  'return a.passwordSupplied ? OpenVerdict::WrongPassword : OpenVerdict::NeedPassword;
    }
    if (a.errorFlags & kErrIsNotArc) return OpenVerdict::NotThisFormat;' \
  'return OpenVerdict::NeedPassword;
    }
    if (a.errorFlags & kErrIsNotArc) return OpenVerdict::NotThisFormat;' \
  'return OpenVerdict::NeedPassword;
    }
    if (a.errorFlags & kErrIsNotArc)'

echo
echo "=========== MUTATION 2: treat undefined flags as corruption ==========="
# Absence of evidence must not be evidence of damage; this mutation makes it so.
# NOTE: the naive form -- replacing the condition with `true` while keeping the
# binding `const bool flagsKnown = a.errorFlags != kErrFlagsUndefined;` -- is a
# SILENT NO-OP. Every test below is `errorFlags & bit` and 0xFFFFFFFF already
# sets every bit, so behaviour is identical and the suite correctly stays green.
# That is the D3 trap in miniature. The mutation therefore rewrites the BINDING
# itself (declaration + init), which is observable.
run_case "m2-undefined-flags-become-corrupt" "$FILE_REL" \
  'const bool flagsKnown = a.errorFlags != kErrFlagsUndefined;' \
  'const bool flagsKnown = true;' \
  'const bool flagsKnown = true;'

echo
echo "======== MUTATION 3: prompt the dialog even with candidates ==========="
# The dialog must only appear when the ring is empty; otherwise it steals focus
# from a probe that would have succeeded silently.
run_case "m3-dialog-with-noncandidates" "$FILE_REL" \
  'if (plan_.ordered.empty()) {' \
  'if (true) {' \
  'if (true) {'

echo
echo "=========== MUTATION 4: forget to redact the outbound text ============"
# Publish() is the single choke point where every message is scrubbed.
run_case "m4-no-redaction-in-publish" "$FILE_REL" \
  '    view_.message = Redact(message_);' \
  '    view_.message = message_;' \
  '    view_.message = message_;'

echo
echo "========= MUTATION 5: let a cancelled run report Exhausted ============"
# Then the user is told "all 12 were wrong" after trying 2 -- a lie that also
# hides the fact that the ring still has usable candidates.
run_case "m5-cancel-reports-exhausted" "$FILE_REL" \
  'if (cancelled_ || out.cancelled) {' \
  'if (false) {' \
  'if (false) {'

echo
echo "===== MUTATION 6: ignore the cancel predicate inside the executor ====="
# The executor is where a cancel must actually stop work.
run_case "m6-executor-ignores-cancel" "$RING_REL" \
  '      if (shouldCancel && shouldCancel()) {' \
  '      if (false && shouldCancel && shouldCancel()) {' \
  'if (false && shouldCancel && shouldCancel()) {'

echo
echo "========= MUTATION 7: progress label loses the 匹配中 prefix ==========="
run_case "m7-progress-label-prefix" "$FILE_REL" \
  '? "匹配中 " + pwd::PasswordRing::ProgressLabel(' \
  '? pwd::PasswordRing::ProgressLabel(' \
  '? pwd::PasswordRing::ProgressLabel('

echo
echo "=========== MUTATION 8: monotone progress guard removed =============="
run_case "m8-progress-not-monotone" "$FILE_REL" \
  '    if (done > done_) done_ = done;' \
  '    done_ = done;' \
  '    done_ = done;'

echo
echo "=========================== RESTORATION =============================="
cd "$SRC" && cmake --build build --target test_open_flow -j4 >/dev/null 2>&1
"$SRC/build/Tests/test_open_flow" 2>&1 | tail -1
