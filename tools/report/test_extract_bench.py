#!/usr/bin/env python3
"""Self-test for tools/report/extract_bench.py -- card C42.

No real benchmark is run: the fixtures are logs in the exact shape 'ctest -V'
produces (captured from a probe project on 2026-11-02, prefix and header order
included), plus one line per way a BENCH line can be broken.

The point of the card is that a WRONG report is worse than no report, so this
test pins BOTH halves:
  * a good log produces the exact table (row-by-row equality, not "contains");
  * every malformed line is reported with its line number and the run is red;
  * a green suite whose numbers never appeared is red (the C32 defect);
  * a raw executable log still parses (the "run the exe directly" alternative).

Fixtures live in their own unique temporary directory (contract: one fixture
directory per test) and are removed afterwards unless --keep is given.

Run standalone:   python tools/report/test_extract_bench.py [-v]
Via ctest:        ctest -R report.extract_bench -V
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

PARSER = Path(__file__).with_name("extract_bench.py")

FAILURES = []
ASSERTIONS = 0


def check(condition, message):
    global ASSERTIONS
    ASSERTIONS += 1
    if not condition:
        FAILURES.append(message)
        print("  FAIL  " + message)
    return condition


def check_eq(actual, expected, message):
    return check(actual == expected,
                 "%s\n         expected: %r\n         actual:   %r"
                 % (message, expected, actual))


# ---------------------------------------------------------------------------
# Fixture construction: the shape 'ctest -V' actually prints
# ---------------------------------------------------------------------------
def ctest_verbose_log(entries):
    """entries: [(name, status, [output lines]), ...] -> ctest -V log text.

    Observed shape (ctest 3.31, Windows, 2026-11-02):

        test 1
            Start 1: win.bench_green

        1: Test command: ...
        1: BENCH {...}
        1/3 Test #1: win.bench_green .........   Passed    0.07 sec

    The stdout comes BEFORE the header, and every output line carries the
    "<n>: " prefix -- both are load-bearing for the parser.
    """
    lines = []
    total = len(entries)
    for index, (name, status, output) in enumerate(entries, start=1):
        lines.append("test %d" % index)
        lines.append("    Start %d: %s" % (index, name))
        lines.append("")
        lines.append("%d: Test command: C:/build/Tests/bench.exe" % index)
        lines.append("%d: Working Directory: C:/build" % index)
        lines.append("%d: Test timeout computed to be: 10000000" % index)
        for out in output:
            lines.append("%d: %s" % (index, out) if out else "%d:" % index)
        dots = "." * max(3, 40 - len(name))
        lines.append("%d/%d Test #%d: %s %s   %s    0.07 sec"
                     % (index, total, index, name, dots, status))
        lines.append("")
    lines.append("100%% tests passed, 0 tests failed out of %d" % total)
    lines.append("")
    return "\n".join(lines)


def raw_executable_log(output_lines):
    """A plain executable log: no ctest headers, no '<n>: ' prefixes."""
    return "\n".join(output_lines) + "\n"


SOLID = 'BENCH {"items":20,"sweep_ms":12.5,"peritem_ms":40.625,"speedup":3.25,"peak_mb":61.5}'
STARTUP = 'BENCH_STARTUP {"exe_to_main_ms":313.6,"first_frame_ms":22.7,"presents":3}'
CORE = 'CORE-BENCH {"items":100,"sweep_decodes":100,"peritem_decodes":5050,"lru_over_budget":0}'
GLASS = 'BENCH_GLASS_MEM {"baseline_mb":41.0,"sevenzip_mb":52.25,"peak_mb":61.5}'
SEEK = '    BENCH {"item_mib":8,"seek_ms":1.5}'  # indented on purpose


def good_entries():
    return [
        ("win.bench_solid", "Passed", [SOLID]),
        ("win.bench_startup", "Passed", [STARTUP]),
        ("core.bench_sweep", "Passed", [CORE]),
        ("win.c33.glass_memory", "Passed", [GLASS]),
        ("win.dnd_seek", "Passed", [SEEK]),
        ("core.noise", "Passed", ["plain test, no numbers"]),
    ]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def run_parser(log_path, *extra):
    return subprocess.run(
        [sys.executable, str(PARSER), str(log_path)] + list(extra),
        capture_output=True, text=True, encoding="utf-8")


def sections(markdown):
    """{'#### name': [(metric, value), ...]} for every table in the output."""
    found = {}
    current = None
    for line in markdown.splitlines():
        if line.startswith("#### "):
            current = line[len("#### "):].strip()
            found[current] = []
            continue
        if current is None or not line.startswith("| "):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if cells == ["metric", "value"] or set("".join(cells)) <= {"-"}:
            continue
        found[current].append(tuple(cells))
    return found


def write_log(directory, name, text):
    path = directory / name
    path.write_text(text, encoding="utf-8", newline="\n")
    return path


def expect_failure(directory, label, text, expected_code, expected_line, needle):
    """A bad log must be red, must name the line, and must print no table."""
    path = write_log(directory, label.replace(" ", "_") + ".log", text)
    result = run_parser(path)
    check_eq(result.returncode, expected_code,
             "%s: exit code (0 would publish a wrong report)" % label)
    check_eq(result.stdout, "", "%s: nothing may reach the summary on failure" % label)
    if expected_line is not None:
        check("%s:%d:" % (path, expected_line) in result.stderr,
              "%s: stderr must point at line %d, got %r"
              % (label, expected_line, result.stderr.strip().splitlines()[:2]))
    check(needle in result.stderr,
          "%s: stderr must say %r, got %r" % (label, needle, result.stderr.strip()))


# ---------------------------------------------------------------------------
# Cases
# ---------------------------------------------------------------------------
def case_good_log(directory):
    print("case: a good ctest -V log -> exact table, exit 0")
    log = write_log(directory, "good.log", ctest_verbose_log(good_entries()))
    summary = directory / "summary.md"
    result = run_parser(log, "--summary", str(summary), "--title", "Benchmarks (selftest)")

    check_eq(result.returncode, 0, "good log: exit code")
    check_eq(result.stderr, "", "good log: stderr must stay empty")
    check("### Benchmarks (selftest)" in result.stdout, "good log: --title is used")
    check("_source: %s -- 5 bench line(s) from 5 suite(s)_" % log in result.stdout,
          "good log: source/counts line")

    tables = sections(result.stdout)
    check_eq(sorted(tables), ["core.bench_sweep", "win.bench_solid", "win.bench_startup",
                              "win.c33.glass_memory", "win.dnd_seek"],
             "good log: one table per emitting suite, in name order")
    check_eq(tables["win.bench_solid"],
             [("items", "20"), ("sweep_ms", "12.5"), ("peritem_ms", "40.625"),
              ("speedup", "3.25"), ("peak_mb", "61.5")],
             "good log: win.bench_solid rows are exactly the payload, in order")
    check_eq(tables["win.bench_startup"],
             [("exe_to_main_ms", "313.6"), ("first_frame_ms", "22.7"), ("presents", "3")],
             "good log: BENCH_STARTUP rows")
    check_eq(tables["core.bench_sweep"],
             [("items", "100"), ("sweep_decodes", "100"), ("peritem_decodes", "5050"),
              ("lru_over_budget", "0")],
             "good log: CORE-BENCH rows")
    check_eq(tables["win.c33.glass_memory"],
             [("baseline_mb", "41.0"), ("sevenzip_mb", "52.25"), ("peak_mb", "61.5")],
             "good log: BENCH_GLASS_MEM rows")
    check_eq(tables["win.dnd_seek"], [("item_mib", "8"), ("seek_ms", "1.5")],
             "good log: indented BENCH line is parsed and attributed")
    check_eq(sum(result.stdout.count("_marker: ") for _ in [0]), 5,
             "good log: one marker caption per line")
    check_eq(summary.read_text(encoding="utf-8"), result.stdout,
             "good log: --summary receives the same bytes as stdout")


def case_bad_lines(directory):
    print("case: every malformed bench line is red, named, and publishes nothing")
    # Each entry: (label, broken line, expected stderr needle)
    broken = [
        ("truncated_json", 'BENCH {"items":20,"sweep_ms":', "malformed JSON"),
        ("not_an_object", 'BENCH [1,2,3]', "not followed by a JSON object"),
        ("non_finite", 'BENCH {"sweep_ms": NaN}', "non-finite"),
        ("empty_object", 'BENCH {}', "EMPTY object"),
        ("marker_without_payload", 'BENCH_SCALE', "has no JSON payload"),
        ("trailing_junk", 'BENCH {"items":20} and then some prose', "trailing text"),
    ]
    for label, broken_line, needle in broken:
        entries = good_entries()
        # Replace the solid suite's good line so the log stays otherwise valid.
        entries[0] = ("win.bench_solid", "Passed", [broken_line])
        text = ctest_verbose_log(entries)
        bad_line = next(i for i, line in enumerate(text.splitlines(), start=1)
                        if broken_line in line)
        expect_failure(directory, label, text, 1, bad_line, needle)

    # A line that only LOOKS like a bench line (source text echoed into a log)
    # must not be mistaken for a broken measurement.
    print("case: prose starting with an unknown BENCH_ token is not a bad line")
    entries = good_entries() + [("core.noise", "Passed", ["BENCH_CHECK(items > 0) failed"])]
    log = write_log(directory, "prose.log", ctest_verbose_log(entries))
    result = run_parser(log)
    check_eq(result.returncode, 0, "prose guard: exit code")
    check("core.noise" not in sections(result.stdout),
          "prose guard: no table for a suite that printed no JSON")


def case_conflicting_duplicate(directory):
    print("case: conflicting duplicate payloads are red (ambiguous numbers)")
    entries = good_entries()
    entries[0] = ("win.bench_solid", "Passed", [SOLID, SOLID.replace("12.5", "99.5")])
    text = ctest_verbose_log(entries)
    second = [i for i, line in enumerate(text.splitlines(), start=1)
              if "99.5" in line][-1]
    expect_failure(directory, "conflicting_duplicate", text, 3, second,
                   "conflicting duplicate")

    print("case: an identical duplicate collapses to one row set")
    entries = good_entries()
    entries[0] = ("win.bench_solid", "Passed", [SOLID, SOLID])
    log = write_log(directory, "identical_duplicate.log", ctest_verbose_log(entries))
    result = run_parser(log)
    check_eq(result.returncode, 0, "identical duplicate: exit code")
    check_eq(len(sections(result.stdout)["win.bench_solid"]), 5,
             "identical duplicate: rows are not doubled")
    # Five emitting suites, one caption each: a second caption would mean the
    # duplicate was published twice.
    check_eq(result.stdout.count("_marker: "), 5,
             "identical duplicate: one caption per suite, not two for the repeat")


def case_green_without_numbers(directory):
    print("case: a Passed bench suite with no BENCH line is red (the C32 defect)")
    entries = good_entries() + [("win.bench_memory", "Passed", ["ran, printed nothing"])]
    text = ctest_verbose_log(entries)
    path = write_log(directory, "silent_suite.log", text)
    result = run_parser(path)
    check_eq(result.returncode, 5, "silent suite: exit code")
    check_eq(result.stdout, "", "silent suite: no table is published")
    check("'win.bench_memory' is reported Passed by ctest but printed no BENCH line"
          in result.stderr, "silent suite: the lost suite is named")


def case_no_numbers_at_all(directory):
    print("case: a log with no BENCH lines is red, never an empty-but-green summary")
    path = write_log(directory, "empty.log", ctest_verbose_log(
        [("win.bench_solid", "Passed", ["nothing"])]))
    result = run_parser(path)
    check_eq(result.returncode, 2, "empty log: exit code")
    check_eq(result.stdout, "", "empty log: no table")
    check("no BENCH lines" in result.stderr, "empty log: says what is missing")
    check("ctest -V" in result.stderr, "empty log: names the fix (ctest -V)")

    blank = write_log(directory, "blank.log", "")
    check_eq(run_parser(blank).returncode, 2, "blank log: exit code")


def case_raw_executable_log(directory):
    print("case: a raw executable log (the direct-exe alternative) still parses")
    path = write_log(directory, "raw.log", raw_executable_log(
        ["noise before", SOLID, "ALL BENCH CHECKS PASSED"]))
    result = run_parser(path)
    check_eq(result.returncode, 0, "raw log: exit code")
    check_eq(sections(result.stdout), {"(unattributed)": [
        ("items", "20"), ("sweep_ms", "12.5"), ("peritem_ms", "40.625"),
        ("speedup", "3.25"), ("peak_mb", "61.5")]},
        "raw log: rows land under (unattributed)")


def case_unreadable(directory):
    print("case: a missing log is exit 4, not an empty table")
    result = run_parser(directory / "does-not-exist.log")
    check_eq(result.returncode, 4, "missing log: exit code")
    check_eq(result.stdout, "", "missing log: no table")
    check("cannot read" in result.stderr, "missing log: says it cannot be read")


def main(argv=None):
    parser = argparse.ArgumentParser(description="Self-test for extract_bench.py")
    parser.add_argument("--keep", action="store_true",
                        help="keep the fixture directory (printed at the end)")
    args = parser.parse_args(argv)

    if not PARSER.is_file():
        print("EXTRACT-BENCH SELFTEST: FAILED -- parser not found at %s" % PARSER)
        return 1

    directory = Path(tempfile.mkdtemp(
        prefix="prism-c42-extract-bench-",
        dir=os.environ.get("PRISM_TEST_TMP") or None))
    print("fixtures: %s" % directory)
    try:
        case_good_log(directory)
        case_bad_lines(directory)
        case_conflicting_duplicate(directory)
        case_green_without_numbers(directory)
        case_no_numbers_at_all(directory)
        case_raw_executable_log(directory)
        case_unreadable(directory)
    finally:
        if args.keep:
            print("fixtures kept: %s" % directory)
        else:
            shutil.rmtree(directory, ignore_errors=True)

    if FAILURES:
        print("EXTRACT-BENCH SELFTEST: FAILED (%d/%d assertions failed)"
              % (len(FAILURES), ASSERTIONS))
        for failure in FAILURES:
            print("  - " + failure.replace("\n", "\n    "))
        return 1
    print("EXTRACT-BENCH SELFTEST: PASSED (%d assertions)" % ASSERTIONS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
