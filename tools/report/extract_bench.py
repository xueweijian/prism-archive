#!/usr/bin/env python3
"""Extract machine-readable BENCH lines from a ctest log into a Markdown table.

Card C42.  The report lane used to run

    ctest --output-on-failure -R "bench"

and ctest does NOT print the stdout of a PASSING test.  So every 'BENCH {json}'
line produced by a green bench suite stayed inside ctest's memory: the summary
step printed "(no BENCH lines)" and the run still looked healthy.  C32 found it;
bench_scale / bench_memory / bench_startup were affected the same way.

The workflow now runs 'ctest -V' (which prints passing stdout) and pipes the log
through this script.  A parser that guesses is worse than no report, so every
failure mode is LOUD:

    exit 0  every bench line parsed, Markdown printed (and appended to --summary)
    exit 1  at least one malformed bench line (bad JSON / not an object / empty
            object / NaN / trailing junk after the object)
    exit 2  no bench lines at all -- exactly the C32 bug, never a success
    exit 3  the same (test, marker) pair appears twice with DIFFERENT payloads
    exit 4  the log (or --summary) cannot be read/written
    exit 5  a bench suite is reported Passed by ctest but printed no BENCH line
            (the report would silently lose that suite)

Grammar.  One marker family, no per-suite special cases:

    BENCH {json}            bench_solid, dnd-seek
    BENCH_STARTUP {json}    bench_startup
    BENCH_MEMORY {json}     bench_memory
    BENCH_SCALE {json}      bench_scale
    BENCH_GLASS_MEM {json}  bench_glass_mem
    CORE-BENCH {json}       core.bench_sweep (any platform)

Markers are matched case-sensitively at the start of a line (leading whitespace
allowed: the dnd-seek line is indented four spaces).  A line that starts with a
KNOWN marker but does not carry one well-formed JSON object is an ERROR, not a
line to skip -- a dropped suite reads exactly like a suite that was never run.
An unknown BENCH_* token followed by something other than '{' is treated as
prose (source text echoed into a log, e.g. a macro name), not as a bench line.

'ctest -V' prefixes every captured output line with the test number:

    1: BENCH {"items":20,"sweep_ms":12.5,...}
    1/3 Test #1: win.bench_green ..........   Passed    0.07 sec

That prefix is stripped before matching (verified against ctest 3.31 on
2026-11-02: without it the marker is never at column 0 and the whole lane would
parse to zero lines).  A raw executable log has no prefix and parses unchanged.

Test attribution.  'ctest -V' and 'LastTest.log' both name the running test, so
each table is titled with the suite that produced it.  A raw executable log
(no ctest headers) still parses; its rows land under '(unattributed)'.

Usage:
    python tools/report/extract_bench.py LOG [--summary PATH] [--title TEXT]
    python tools/report/extract_bench.py - < log          # stdin
"""

import argparse
import json
import re
import sys

# ---------------------------------------------------------------------------
# Grammar
# ---------------------------------------------------------------------------
# The known emitters.  Their lines MUST be well formed (a broken one is red).
KNOWN_MARKERS = frozenset({
    "BENCH",
    "BENCH_STARTUP",
    "BENCH_MEMORY",
    "BENCH_SCALE",
    "BENCH_GLASS_MEM",
    "CORE-BENCH",
})
# Any marker-shaped token, so a NEW suite is published without touching this
# file.  If its line is malformed it is still caught by the "Passed but no
# BENCH line" check (exit 5) rather than being dropped.
MARKER = r"(?:CORE-BENCH|BENCH(?:_[A-Z0-9]+)*)"
BENCH_LINE = re.compile(r"^(?P<indent>\s*)(?P<marker>" + MARKER + r")\b(?P<rest>.*)$")

# ctest console:  "3/7 Test #3: win.bench_solid .........   Passed   12.34 sec"
# LastTest.log:   "2/5 Test: win.bench_solid" / "2/5 Testing: win.bench_solid"
CTEST_HEADER = re.compile(
    r"^\s*(?:\d+/\d+\s+)?Test\s+#(?P<num>\d+):\s+(?P<name>\S+)(?P<tail>.*)$")
CTEST_LOG_HEADER = re.compile(
    r"^\s*(?P<num>\d+)/\d+\s+(?:Test|Testing):\s+(?P<name>\S+)\s*$")
STATUS_WORD = re.compile(r"\b(Passed|Failed|Timeout|Skipped|Not Run|Exception)\b")
# 'ctest -V' / LastTest.log prefix each captured output line with "<n>: ".
CTEST_OUTPUT_PREFIX = re.compile(r"^\s*(?P<num>\d+):\s?(?P<content>.*)$")

# A suite whose numbers the lane promised to publish.
BENCH_TEST_NAME = re.compile(r"(?:^|\.)bench")

BAD = "bad"
DUP = "dup"
MISSING = "missing"


class BadBenchLine(Exception):
    """A bench line that cannot be trusted."""


def _reject_constant(token):
    raise BadBenchLine("non-finite number %r is not valid JSON" % token)


def parse_payload(marker, rest):
    """Parse the JSON object that must follow the marker.  Raises BadBenchLine."""
    body = rest.strip()
    if not body:
        raise BadBenchLine("marker %s has no JSON payload" % marker)
    if not body.startswith("{"):
        raise BadBenchLine("marker %s is not followed by a JSON object: %r"
                           % (marker, body[:60]))
    decoder = json.JSONDecoder(parse_constant=_reject_constant)
    try:
        value, end = decoder.raw_decode(body)
    except BadBenchLine:
        raise
    except ValueError as exc:
        raise BadBenchLine("malformed JSON after %s: %s" % (marker, exc))
    trailing = body[end:].strip()
    if trailing:
        raise BadBenchLine("trailing text after the %s object: %r" % (marker, trailing[:60]))
    if not isinstance(value, dict):
        raise BadBenchLine("marker %s carries %s, not a JSON object"
                           % (marker, type(value).__name__))
    if not value:
        raise BadBenchLine("marker %s carries an EMPTY object -- a table with no rows "
                           "is not a measurement" % marker)
    return value


def scan_log(lines):
    """Return (suites, tests_seen, problems).

    suites:      ordered {test_name: [(marker, payload, line_no), ...]}
    tests_seen:  {test_name: status_word_or_None} for every ctest header found
    problems:    [(kind, line_no, message), ...]

    Two passes, because 'ctest -V' prints a test's stdout BEFORE its
    "N/M Test #K: name ... Passed" line.  Pass 1 maps test numbers to names (the
    "<n>: " output prefix carries the number, so attribution does not depend on
    line order at all); pass 2 parses the bench lines.
    """
    number_to_name = {}
    tests_seen = {}
    for raw in lines:
        line = raw.rstrip("\r\n")
        header = CTEST_HEADER.match(line)
        if header:
            number_to_name[header.group("num")] = header.group("name")
            status = STATUS_WORD.search(header.group("tail"))
            tests_seen.setdefault(header.group("name"), None)
            if status:
                tests_seen[header.group("name")] = status.group(1)
            continue
        log_header = CTEST_LOG_HEADER.match(line)
        if log_header:
            number_to_name.setdefault(log_header.group("num"), log_header.group("name"))
            tests_seen.setdefault(log_header.group("name"), None)

    suites = {}
    problems = []
    current = None
    for index, raw in enumerate(lines, start=1):
        line = raw.rstrip("\r\n")
        header = CTEST_HEADER.match(line)
        if header:
            current = header.group("name")
            continue
        log_header = CTEST_LOG_HEADER.match(line)
        if log_header:
            current = log_header.group("name")
            continue
        # 'ctest -V' writes "<n>: BENCH {...}"; a raw executable log does not.
        prefixed = CTEST_OUTPUT_PREFIX.match(line)
        content = prefixed.group("content") if prefixed else line
        test = None
        if prefixed:
            test = number_to_name.get(prefixed.group("num"))
        bench = BENCH_LINE.match(content)
        if not bench:
            continue
        marker = bench.group("marker")
        if not bench.group("rest").strip().startswith("{") and marker not in KNOWN_MARKERS:
            continue  # prose that merely begins with an unknown BENCH_ token
        try:
            payload = parse_payload(marker, bench.group("rest"))
        except BadBenchLine as exc:
            problems.append((BAD, index, str(exc)))
            continue
        test = test or current or "(unattributed)"
        entries = suites.setdefault(test, [])
        duplicate = False
        for seen_marker, seen_payload, _ in entries:
            if seen_marker != marker:
                continue
            duplicate = True
            if seen_payload != payload:
                problems.append((
                    DUP, index,
                    "conflicting duplicate %s for %r: %s vs the earlier %s"
                    % (marker, test,
                       json.dumps(payload, sort_keys=True),
                       json.dumps(seen_payload, sort_keys=True))))
            break
        if not duplicate:
            entries.append((marker, payload, index))
    return suites, tests_seen, problems


def _cell(value):
    """Render one value for a Markdown table cell."""
    if isinstance(value, bool):
        text = "true" if value else "false"
    elif isinstance(value, float):
        text = repr(value)
    elif isinstance(value, (int, str)) or value is None:
        text = "null" if value is None else str(value)
    else:
        text = json.dumps(value, separators=(",", ":"), sort_keys=True)
    return text.replace("|", "\\|").replace("\n", " ")


def render(suites, source, title):
    """Markdown for the step summary.  Byte-stable for the same input."""
    total = sum(len(entries) for entries in suites.values())
    out = ["### " + title, ""]
    out.append("Report-only digits: a shared runner cannot gate a stopwatch "
               "(see docs/PERF.md).")
    out.append("")
    out.append("_source: %s -- %d bench line(s) from %d suite(s)_"
               % (source, total, len(suites)))
    for test in sorted(suites):
        out.append("")
        out.append("#### %s" % test)
        for marker, payload, line_no in suites[test]:
            out.append("")
            out.append("_marker: %s (log line %d)_" % (marker, line_no))
            out.append("")
            out.append("| metric | value |")
            out.append("|---|---|")
            for key, value in payload.items():
                out.append("| %s | %s |" % (_cell(key), _cell(value)))
    out.append("")
    return "\n".join(out) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Parse BENCH {json} lines from a ctest -V log into Markdown.")
    parser.add_argument("log", help="ctest -V log, or - for stdin")
    parser.add_argument("--summary", metavar="PATH",
                        help="append the Markdown to this file (e.g. $GITHUB_STEP_SUMMARY)")
    parser.add_argument("--title", default="Benchmarks (report lane)",
                        help="Markdown heading text")
    args = parser.parse_args(argv)

    if args.log == "-":
        lines = sys.stdin.read().splitlines(keepends=True)
        source = "<stdin>"
    else:
        try:
            with open(args.log, "r", encoding="utf-8", errors="replace") as handle:
                lines = handle.readlines()
        except OSError as exc:
            print("EXTRACT: FAILED -- cannot read %s: %s" % (args.log, exc), file=sys.stderr)
            return 4
        source = args.log

    suites, tests_seen, problems = scan_log(lines)

    # No numbers at all is the C32 defect itself, and it must not look like a
    # successful run that simply had nothing to say.
    if not suites:
        print("EXTRACT: FAILED -- no BENCH lines in %s.  A green bench suite whose "
              "numbers never reached the report is exactly the defect this lane "
              "exists to remove (card C32); run the suites with 'ctest -V'."
              % args.log, file=sys.stderr)
        return 2

    failures = list(problems)
    # A green bench suite with no BENCH line means the summary silently lost that
    # suite -- the same defect, one suite at a time.
    for name in sorted(tests_seen):
        if tests_seen[name] != "Passed" or not BENCH_TEST_NAME.search(name):
            continue
        if not suites.get(name):
            failures.append((MISSING, 0,
                             "%r is reported Passed by ctest but printed no BENCH line"
                             % name))

    if failures:
        for kind, line_no, message in failures:
            where = "%s:%d" % (args.log, line_no) if line_no else args.log
            print("EXTRACT: FAILED -- %s: %s" % (where, message), file=sys.stderr)
        print("EXTRACT: FAILED (%d problem(s)); nothing was written to the summary"
              % len(failures), file=sys.stderr)
        kinds = {kind for kind, _, _ in failures}
        return 1 if BAD in kinds else (3 if DUP in kinds else 5)

    markdown = render(suites, source, args.title)
    sys.stdout.write(markdown)
    if args.summary:
        try:
            with open(args.summary, "a", encoding="utf-8") as handle:
                handle.write(markdown)
        except OSError as exc:
            print("EXTRACT: FAILED -- cannot append to %s: %s" % (args.summary, exc),
                  file=sys.stderr)
            return 4
    return 0


if __name__ == "__main__":
    sys.exit(main())
