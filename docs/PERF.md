# Performance Benchmark & Invariants (blueprint ch. 7.2)

PrismArchive's solid-block claim — *one linear sweep beats per-item extraction* —
is stated here as something a CI run can prove, not as an adjective. Two
complementary artifacts carry that proof:

| Artifact | Where it runs | What it proves |
|---|---|---|
| `Tests/bench_sweep.cpp` (`core.bench_sweep`) | every lane, every push (Linux, seconds) | the **shape** of the work: exact decode/decompression counts, block-size-independent peak memory, abandon contract, LRU budget, progress monotonicity — each with a negative control |
| `Tests/win/bench_solid.cpp` (`win.bench_solid`) | Windows lane (real 7z.dll + real solid archive) | the **wall-clock** confirmation: sweep vs per-item median-of-3, real pixels out, published as CI digits |

## What is measured, and why invariants instead of absolute times

CI runners are shared and noisy: "sweep took 12 ms" is a different number on
every run and every machine, so it can never be a merge gate. What *is* a
property of the algorithm — and therefore gateable — is the shape of the work:

1. **Decode count.** With a counting fake decoder, one linear pass over N items
   issues **exactly N** decodes. Per-item extraction of a solid block must
   replay the prefix, so N items cost **exactly N·(N+1)/2** item
   decompressions — asserted bit-exact (100 items → 5050, not "~5000").
2. **Peak memory is independent of block size.** Measured with a real
   `operator new` counter (not a model): sweeping a ~1 MB block and a ~100 MB
   block produces the **identical** peak, because the sweep never materialises
   the block — peak = scratch cap + thumbnail budget.
3. **Abandon stops decoding, exactly.** Abandoning mid-block yields
   **precisely** `cut` decodes (verified at *every* split point of a 12-item
   block) while the stream stays fully drained — the 7z callback contract.
4. **LRU budget holds under any op sequence.** 1000 fixed-seed random
   insert/lookup/touch rounds: `SizeBytes() <= BudgetBytes()` after *every*
   step. Fixed seed → reproducible red on any machine.
5. **Progress is monotone and bounded.** Under 1000 random legal extractor
   sequences, `Fraction()` never decreases and stays in [0,1].

**Why this beats a stopwatch:** an invariant failing means a *specific
regression* reappeared — someone buffered the block, someone deleted the
eviction loop, someone lets the extractor decode past abandon. The suite names
which one. A wall-clock regression names nothing.

## The Kaggle matrix: 44 configurations, and what it actually says

`kaggle/bench_solid.py` sweeps N ∈ {10, 50, 200} × file size ∈ {8KB, 256KB, 4MB}
× `-mx` ∈ {1,5,9} × `-ms` ∈ {on, off}, median-of-3, on a dedicated kernel
(`weijianxue/prism-bench-matrix`) so the digits do not depend on the phone
sandbox or a shared CI runner. 44/44 configurations completed, 0 pruned.

### Two measurement bugs found before any number was believed

The first version of this matrix was wrong in two ways that made every digit
meaningless, and both were caught by measuring rather than reading:

1. **Packing ran without `cwd`.** `7z a ... .` resolves `.` against the
   *calling process's* directory, so it packed the entire repository. Measured
   side by side on the same 20 files: **0.17 MB with `cwd`, 159.64 MB
   without** — a 940× error in archive size, which is also the input to both
   timing arms. Fixed, and `run()` now documents why `cwd` is not optional.
2. **`archive_names()` returned the archive itself as entry 0**, so the
   per-item arm ran one extra `7z e` that opened the archive rather than an
   item. Negative control: removing the self-row filter takes a 10-file
   fixture from 10 names to 11.

A calibration was also run on the per-item arm's own convention (`7z e -so`,
stdout discarded): compared against actually writing each item to disk, the
`-so` form understates real per-item cost by **2%** — so that arm is honest,
and it is the *conservative* one.

### Folders-first, and the number that was hiding

The headline claim — solid blocks make a sweep beat per-item extraction — is
**true but incomplete**, because the ratio conflates two effects:

* **N**: per-item extraction pays process spawn N times. At N=200 with 8 KB
  files that alone yields 15.3× even with `-ms=off`.
* **Solid**: the prefix replay per item, which is the effect we actually claim.

Isolating it by holding N and file size fixed and comparing `per_item(ms=on)`
against `per_item(ms=off)` — the cost solid blocks *add* to the per-item path:

| N | 8 KB | 256 KB | 4 MB |
|---|---|---|---|
| 10 | 1.2× | 2.3× | 4.3× |
| 50 | 1.4× | 6.5× | 6.5× |
| 200 | 1.7× | **21.4×** | (budget) |

So solid mode makes per-item extraction **1.2×–21.4× slower**, and the penalty
grows with item size — exactly the prefix-replay cost the sweep avoids. Reporting
only `peritem/sweep` hides that; reporting both makes the claim falsifiable.

Sweep-vs-per-item by N (median over all sizes and levels), which is what a user
feels when selecting the whole archive:

| N | `-ms=off` | `-ms=on` | net solid effect |
|---|---|---|---|
| 10 | 1.5× | 6.7× | 4.3× |
| 50 | 1.6× | 33.2× | **20.1×** |
| 200 | 15.5× | 63.7× | 4.1× |

The N=200 row is the honest anomaly: at 8 KB the spawn cost dominates `-ms=off`
so the *net* solid effect looks small, and the 4 MB cells were cut by the
kernel's time budget. Both are stated rather than smoothed.

## Negative controls (anti-fake-green)

Every invariant ships with a deliberately broken implementation that the same
predicate must reject — the suite cannot stay green while being decorative:

- block-buffering arm → violates the peak-memory invariant (peak scales with the block);
- decode-after-abandon arm → violates the decode-count contract;
- `UnboundedCache` (eviction "optimised away") → breaches the budget predicate;
- per-item-reset fraction → caught by the monotonicity predicate.

**Rehearsal note:** the uncommitted `Gallery.hpp` change found during this work
was exactly regression (4) — the eviction loop in `ThumbnailLru::Insert` had
been removed. It was reverted (product code is not modified by benchmarks), and
re-injecting it verbatim was verified to turn
`LRU budget holds across 1000 seeded random op sequences` red (`exit 1`). The
control is real, not theoretical.

## The real 7z.dll comparison (Windows lane)

`win.bench_solid` packs 20 real PNGs (5 size classes, 64×64 … 640×480) into a
genuine solid 7z via the 7-Zip CLI, then times, with `QueryPerformanceCounter`,
median of **3 interleaved repetitions**:

- **Arm A (sweep):** one `Extract` call over the block — all 20 thumbnails harvested;
- **Arm B (per-item):** 20 separate extractions — each re-decodes the solid prefix.

Both arms must decode all 20 items into real BGRA pixels (checked) or the test
fails; the timing digits are printed, never asserted (see "why invariants" —
the *guarantee* is the arithmetic in `core.bench_sweep`; the Windows number is
the honest confirmation on the real pipeline). Missing `7z.exe` is a **failure**
(`exit != 0`), never a silent skip.

One machine-readable line per run:

```
BENCH {"items":20,"sweep_ms":..,"peritem_ms":..,"speedup":..,"peak_mb":..}
```

`speedup = peritem_ms / sweep_ms` (median of 3). `peak_mb` is the process peak
working set (`GetProcessMemoryInfo`). The line is appended to the GitHub step
summary by the `Performance benchmark -> step summary` CI step.

## CI numbers

> Filled automatically by CI (`Performance benchmark -> step summary`). To
> refresh locally: run the repro commands below on a Windows machine.

| Metric | Value |
|---|---|
| sweep_ms | _from CI_ |
| peritem_ms | _from CI_ |
| speedup | _from CI_ |
| peak_mb | _from CI_ |
| core: sweep_decompressions (N=100) | 100 |
| core: peritem_decompressions (N=100) | 5050 |
| core: peak_bytes 1 MB vs 100 MB block | identical (≈339 KB) |
| core: lru_over_budget across 1000 rounds | 0 |

## Reproduce

```sh
# Core invariants + fake-decoder counters (any OS, no dependencies)
cmake -S . -B build -DPRISM_BUILD_WIN_LAYER=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --output-on-failure        # includes core.bench_sweep
./build/Tests/bench_sweep        # prints the CORE-BENCH JSON line

# Windows wall-clock benchmark (needs 7z.exe beside 7z.dll)
set PRISM_7Z_DLL=C:\Program Files\7-Zip\7z.dll
ctest -R win.bench_solid --output-on-failure
```

## ch.7.2 matrix, cell by cell (measured 2026-11-01)

**Machine**: Windows 11 26200 (Home, zh-CN), Intel i5-8250U (4 cores / 8 threads),
7-Zip 25.01 (`E:\DevTools\7-Zip\7z.dll`), VS 2022 17.14 (MSVC 14.44), Release.
**The box is shared** with seven sibling worktrees compiling in parallel, so every
disk-bound digit is a floor, not a best case; the cells where that matters say so.

**Re-measured after rebasing onto `main`** (C1 lifecycle, C2 gates, C3
distribution, C4 shell, C5 security, C6 glass, C11 scheduling all merged; the
tables are the `2e278ff` run, then the branch was re-rebased onto `ca08457` for
C4 -- C4 adds shell targets only, `ninja` reports `PrismArchive.exe` unchanged,
so the black-box digits still describe the current sources). The tables carry
the **17:3x-17:5x run**; where the earlier 16:0x run differs materially its digit
is in parentheses, because the difference is machine state (sibling builds, page
cache, AV scanning), not a code change. One cell changed for a real reason and
says so: close latency, where C1's worker-join fix landed.
A later re-run of startup/memory during a sibling-compile storm read 37.6 ms
first frame / 517.2 ms close / 45.1 MB idle -- recorded as the load sensitivity
of those cells, not used as their value.
Reproduce with one command:

```powershell
tools/bench/run-perf.ps1            # build + run startup / memory / scale
```

Every suite prints one machine-readable line (`BENCH_STARTUP`, `BENCH_MEMORY`,
`BENCH_SCALE`) and pins it with a ctest `PASS_REGULAR_EXPRESSION`, so a silent
SKIP can never count as a pass (contract D1). **The digits are report-only**: the
only duration assertions in the code are hang detectors (first frame < 5 s, list
frame < 33 ms). A shared 4-core machine cannot gate a stopwatch.

### 1. Startup

| Metric | Measured | How |
|---|---|---|
| process start -> `main` | **313.6 ms** (earlier run 156.9 ms; coarse: `GetProcessTimes` FILETIME quantum is 15.6 ms) | `MsSinceProcessStart()` |
| `RegisterClassExW` + `CreateWindowExW` | **5.0 ms** (3.2-27.1 ms across runs) | QPC around the calls |
| window -> **first frame** (`BeginDraw`+`EndDraw`+`Present`+`Commit`, 3 glass islands, real D3D11 + D2D + DComp) | **22.7 ms** (earlier run 34.3 ms) | QPC around the 3-island paint |
| second frame (steady state, one island) | **11.1 ms** (12.1 ms) | same path |
| 7z.dll `LoadLibrary` (first use in process) | **0.88 ms** (1.31 ms) | `SevenZipLibrary::Load` |
| format table (`GetNumberOfFormats`, 60 formats) | **< 0.001 ms** | QPC |
| **engine first touch**: `CreateObject(IInArchive)` then `Open` | **4.9 ms + 1.8 ms = 6.7 ms** this run; earlier 84.1 ms inside `Open`, and 2377.7 ms in `CreateObject` + 2.0 ms in `Open` on another | see the note below |
| `ReadCatalog` (7 items, no payload) | **0.057 ms** | QPC |

**The 6.7 ms vs 2.4 s split is a finding, not a footnote.** The same total work
(create the 7z handler + parse the header) was observed as **6.7 ms** in this
run (warm image, quiet box), as **84 ms** inside `Open` on one earlier run, and
as **2.4 s** in `CreateObject` + 2 ms in `Open` on another. The first touch of
the engine image is dominated by page faults / AV scanning when the box is busy,
and *which* call pays for it moves. Conclusion recorded for the next pass: the
engine's cold first touch spans **6.7 ms - 2.4 s** and cannot be attributed to
our code from these runs; a dedicated machine (or an ETW/latency trace) is
needed -- and it is the single biggest obstacle to the 12 ms right-click cell.

Verdicts:

* **first frame <= 60 ms (ADR-001 target)**: **22.7 ms -> 达标** on a quiet box
  (34.3 ms on the earlier run). Under full disk saturation by the sibling builds
  the same measurement reads 347.7 ms; the number is an idle-machine floor and is
  labelled as such.
* **cold start <= 25 ms (blueprint Phase 4)**: **313.6 ms -> 未达标** (156.9 ms on
  the earlier run). The process is not in `main` until 157-314 ms after
  `CreateProcess`, before a single line of ours runs: loader + import resolution
  (d3d11/d2d1/dcomp/dwrite/windowcodecs + 7z) plus the machine's AV scanning the
  image. Our own first frame is 22.7 ms. Reaching 25 ms end-to-end needs the
  blueprint's pre-warmed resident process ("后台常驻轻量预热"), which does not
  exist. That is the reason, not a shrug.
* **right-click < 12 ms (7.2 row 2)**: **未达标 / 不在本卡**. The shell-extension
  lane (`PrismShellExt`) belongs to card c2. What is measured here is the engine
  decomposition a right-click pays: 0.88 ms load + 60-format table (<1 us) +
  handler creation + `Open` = **6.7 ms on a warm image, 84 ms - 2.4 s cold**.
  The warm floor alone would fit in 12 ms, but the cold path does not, and the
  shell-side cost is not measured here -- so the cell is not claimed.
* **close latency**: `WM_CLOSE` -> process gone on the SHIPPED
  `PrismArchive.exe` = **153.0 ms** (measured 17:4x on the current binary), down
  from **778 ms** quiet / 1766 / 2318 / 3122 ms under disk saturation in the
  pre-fix build, measured black-box (`PostMessageW(WM_CLOSE)` to the real
  window, then wait on the process handle). The 500 ms product budget asserted
  by `test_app_lifecycle` (`WM_CLOSE` -> `Run()` returns) is met, and this is
  the stricter end-to-end version (process gone, not just `Run()` returned).
  Under sibling compile load the same probe read **517.2 ms**, so the budget is
  met on an idle box, not guaranteed under full load -- both digits recorded
  rather than only the friendly one.
  The improvement is C1's worker-join fix (PR #21), now in `main`. The window
  appears **800.8 ms** after `CreateProcess` on this run (497 ms when the disk
  was idle, 1106 ms when not). The exit-time `CrashRpt1500.dll_unloaded` fault
  recorded against the pre-fix build (BEX64 / 0xc0000005) does **not** appear in
  the Application log for any of this run's launches.

### 2. Memory

| Metric | Measured | Note |
|---|---|---|
| idle working set, engine untouched (benchmark process) | **6.6 MB** | `GetProcessMemoryInfo`, 300 ms after start |
| **idle working set, shipped `PrismArchive.exe`** | **58.4 MB** (earlier run 47.2 MB) | black box: launch on a 7-item archive, 20 samples after 2 s |
| `Open` + catalog of the 100 MB archive | 9.7 MB peak (9.8 MB) | sampled every 2 ms |
| 100 MB archive, **full extract to disk** | **10.6 MB peak**, 104 864 781 bytes (9 files), **423.5 ms** (approx 248 MB/s; earlier run 262.9 ms) | streams through `DiskOutStream` |
| sweep peak, **1 MiB** payload | **10.5 MB**, 6.0 ms (11.8 MB, 16.9 ms) | one solid block, 8 thumbnails decoded |
| sweep peak, **100 MiB** payload | **9.9 MB**, 18.1 ms (11.0 MB, 36.6 ms) | same 8 thumbnails, 100x the payload |
| process peak working set (whole run) | 11.8 MB (11.9 MB) | `PeakWorkingSetSize` |

Verdicts:

* **空闲内存 <= 24/25 MB (ADR-001 / Phase 4)**: **58.4 MB -> 未达标** (47.2 MB
  before the Liquid Glass UI landed, i.e. the glass layer added ~11 MB). Where it
  goes: D2D1 + DComp + DWrite + WIC factories, the 7z.dll image and codec tables,
  the catalog copy, the renderer's resources. The benchmark process itself sits
  at 6.6 MB, so ~52 MB is the real UI stack, not bloat removable by deleting
  code. The number is now a recorded baseline -- exactly what ADR-001 asked for
  instead of the old "<= 24 MB" promise.
* **extract 100 MB peak**: **10.7 MB -> 达标** (the promise is "streams, does not
  slurp"; there is no blueprint bound).
* **thumbnail scan peak independent of block size (C2)**: **11.8 MB vs 11.0 MB**
  (a 0.8 MB *decrease* with a 100x payload) across the same 8 thumbnails ->
  **达标**. Proven bit-exactly by `core.bench_sweep` (fake decoder, 1 MB vs
  100 MB block); here it is confirmed on the real 7z.dll. Two honest limitations:
  (a) with `-mx0` (store) the copy coder lets 7-Zip **skip** the unrequested
  100 MiB item by seeking, so that payload never enters our `ScratchStream`
  (visible in the JSON as `sweep_*_drained_mb: 0` -- the content check for these
  arms is the decoded-thumbnail count, 8/8, not the drained bytes); (b) the
  block-buffering mutant proves the predicate is sharp, not that the item
  streamed.

### 2.1 C33: where the 58.4 MB goes, one stage at a time (measured 2026-11-01, `feat/c33-memory`)

**How**: `win.c33.glass_memory` (`Tests/win/bench_glass_mem.cpp`) builds the
graphics stack one layer at a time through the same API sequence
`GlassIslandHost::Initialize` performs, and prints the working-set delta of
each stage (`GetProcessMemoryInfo`, median of nine reads after a settle). The
device is **WARP on purpose**: `GetProcessMemoryInfo` only sees system memory,
so a hardware device would park the D3D/DComp/effect allocations in VRAM and
the table would be empty. These digits are therefore the **upper bound of what
the glass layer can cost in *process* memory** — which is exactly the number
that matters on any machine that falls back to WARP, and the app does fall back.

| Stage | Working set | Delta | Note |
|---|---|---|---|
| baseline (COM only) | 8.51 MB | — | |
| + `7z.dll` mapped, no handler yet | 9.24 MB | **+0.71 MB** | image itself is 1.88 MB (`SizeOfImage`) |
| + D3D11 device (WARP) | 12.92 MB | **+3.68 MB** | |
| + D2D1 factory / device / context | 13.69 MB | **+0.77 MB** | |
| + DWrite factory | 13.81 MB | **+0.12 MB** | DWrite is *not* a big fixed cost |
| + DComp device | 13.99 MB | **+0.18 MB** | |
| + 1600x1000 x2 composition buffers (12.8 MB nominal) | 14.32 MB | **+0.33 MB** | DXGI commits flip buffers lazily: allocating is cheap, presenting is not |
| + `GlassIslandHost` device stack (same stack + 2 HLSL compiles + 2 effect registrations) | 27.51 MB | **+14.06 MB** | the product object, created all at once |
| + glass graph live, 3 passes @1600x1000 | 67.84 MB | **+40.33 MB** | 18.31 MB is the three intermediate passes; the rest is the two 1600x1000 device bitmaps + D2D pool padding |
| graph released (nothing visible, incl. `IDXGIDevice3::Trim`) | 68.00 MB | **+0.16 MB** | see finding 2 |
| graph re-acquired (drawn again) | 68.01 MB | +0.01 MB | |

**Findings (each one a number, not an adjective):**

1. **The glass layer can cost 40 MB, not 11 MB.** One full-window island
   (1600x1000) with the two-pass Kawase + LiquidGlass graph adds **+40.33 MB**
   of process memory on WARP. C7 attributed +11 MB to the glass UI on the real
   machine; on hardware most of this is VRAM, but the *shape* is the same — and
   the layer was allocating at window size when a pill only ever needs its own
   rect.
2. **Releasing the effect instances returns 0 bytes.** Dropping the three
   `ID2D1Effect` instances leaves the working set unchanged (67.85 -> 67.85 MB):
   D2D keeps the intermediate surfaces in the device context's own target pool.
   `IDXGIDevice3::Trim()` — the documented "this process is idle, you may drop
   its cached surfaces" call — is what turns the release into memory, and even
   then the yield depends on what else still references the pool: **0.16 MB**
   right after a fresh full-window draw, **17.81 MB** one cycle later (Trim on an
   otherwise idle graph). The lazy path in
   `GlassIslandHost::ReleaseTransient` therefore calls both, and the *assertion*
   is on the accounting (derived from live COM state), not on this digit.
3. **`AddIsland` is not where the memory is.** Two composition buffers at
   1600x1000 (12.8 MB nominal) cost +0.33 MB until painted.
4. **The engine is 0.71 MB.** 7z.dll is 1.88 MB on disk and costs +0.71 MB
   resident with no handler created — it is not the 58 MB story.
5. **ThumbStore's 64 MiB budget saturates exactly and an idle `Trim` returns
   the pixels** (D17, both halves):

| ThumbStore (64 MiB budget, 300 KiB thumbs) | Measured |
|---|---|
| resident after a 218-thumb sweep | **63.87 MB** (cap reachable: 139 264 B slack < one thumb) |
| resident after `Trim(1/8 budget)` | **7.91 MB**, returned **55.96 MB** exactly |
| resident after `ReleaseAll()` | **0 B**, 0 entries |
| resident > budget after any op of a 1000-op seeded sequence | **0 times** (`core.test_thumb_budget`) |

Before this card nothing ever *returned* those pixels: the budget was a cap that
only pressure could move, so a gallery that once filled 64 MiB kept 64 MiB for
the rest of the session. `ThumbStore::Trim`/`ReleaseAll` are the idle path; the
call site (minimise / hide) lives in the window, not in this card.

**The frame budget (120 FPS), decomposed.** 100 000 rows, 120 frames,
1200x800 software target, the real `GalleryRenderer`:

| 100k-row list frame | median | p95 |
|---|---|---|
| record (`Draw`) | **4.149 ms** | **11.345 ms** |
| rasterise/flush (`EndDraw` — the only presentation reachable headlessly) | **5.211 ms** | **14.335 ms** |
| total | **11.565 ms** (86.5 FPS) | **20.081 ms** (49.8 FPS) |

**Verdict: 120 FPS 未达标 at p95** — four runs of this card read p95
**11.6 / 20.1 / 45.3 / 127.7 ms** (median 7.4 / 11.6 / 14.6 / 19.3 ms) against
the 8.33 ms budget: the median met it in 3 of the 4 runs, the p95 in **none**.
Where it sticks: the **flush half is 45-71 % of the p95 frame** (14.34 of
20.08 ms on the run above, 7.38 of 16.51 ms on the clean one) and it is the
half the renderer cannot tune — D2D rasterising a full 1200x800
surface, because this harness (like C7's) has no window to present to
(`Present` belongs to C1). Cutting it needs a GPU target, where `EndDraw`
submits rather than rasterises; the recorder itself (3.8-6.6 ms) is already
inside the budget. The run-to-run spread is machine load — seven sibling
worktrees were compiling during the 45.3/127.7 ms runs — and is recorded rather
than smoothed.

**Negative controls** (CONTRACTS D3, registered without `WILL_FAIL` per D26 —
the marker is the conjunction "the run went red **and** the injected branch was
observed at runtime"; see `Tests/suites/c33-memory.cmake`):

| Mutant | What it injects | What went red / evidence |
|---|---|---|
| `mutation.c33.lazy_release` | `PRISM_GLASS_MUTANT_NO_RELEASE`: `ReleaseInstances()` returns 0 and keeps the graph | `expected true: !glass.liveAfterRelease`, `transientAfterRelease == 0`; runtime sign: the graph is still live after `SetVisible(false)` -> `LAZY-RELEASE MUTATION CAUGHT` |
| `mutation.c33.thumb_trim` | `PRISM_THUMB_MUTANT_NO_TRIM`: `Trim()` returns 0 and drops nothing | `expected true: idle trim brings resident down to the target`, `trim reports exactly the bytes it returned`; runtime sign: `Trim` returned 0 with resident > target -> `THUMBSTORE-TRIM MUTATION CAUGHT` |

### 3. Scale

| Metric | Measured | Note |
|---|---|---|
| **10 000-entry** archive `Open` | **7.5 ms** (earlier run 4.0 ms; 21.6 ms under load) | 10 000 real files packed by `7z.exe` |
| **10 000-entry** `ReadCatalog` | **7.1 ms** (earlier 3.6 ms; 9.3 ms under load) | 0.71 us/entry; the catalog count is asserted == 10 000 |
| **4 GiB** archive `Open` (one stored item) | **78.5 s** (earlier 53.5 s) | suspected: the ZIP candidate probe scans the file; see below |
| **4 GiB** archive, full sweep | **73.5 s** (earlier 93.5 s) | 4 GiB drained through 7z.dll |
| **4 GiB** archive, full extract to disk | **176.6 s** (approx 24.3 MB/s; earlier 244.0 s) | 4 294 967 296 bytes written |
| files created in a **controlled `%TEMP%`** during the 4 GiB extract | **0 -> 0** | `TEMP`/`TMP` redirected to a dedicated empty dir; full tree snapshot (paths + sizes) before/after |
| **100 000-row** list, real `GalleryRenderer`, 1200x800 software target | **7.9 ms/frame** median, p95 15.6 ms -> **125.9 FPS** CPU-side (earlier 5.6 ms / p95 8.0 / 178.2 FPS) | 120 frames spread across the whole list |
| **1 000-row** list, same harness | 7.8 ms/frame (5.8 ms) | the point: the cost does **not** depend on N |
| rows drawn per frame at 1 000 vs 100 000 rows | **25 vs 25** (viewport bound) | asserted equal; virtualization is exact |

Verdicts:

* `ReadCatalog` is **7.5 ms for 10 000 entries** (4.0 ms on the earlier run),
  not the "about a millisecond for 50 000" the header comment in
  `PrismEngineBridge.hpp` promises -- roughly a 10-40x gap. The read itself is
  linear and payload-free (0 bytes decompressed). The comment is not this card's
  file: recorded so the claim gets corrected or the code gets faster.
* **Opening the 4 GiB archive takes 78.5 s -> 未达标** for any interactive
  promise (ADR-001's 60 ms first frame is irrelevant if the catalog is not
  there). 78.5 s for a 4 GiB file is ~55 MB/s of sequential read, i.e. exactly a
  whole-file scan at this disk's speed (the earlier run read 53.5 s, ~80 MB/s).
  The arithmetic points at the candidate-handler order: `Open` tries the ZIP
  handler first (`openTrace` shows `Zip.Open hr=0x00000001` = S_FALSE). That is
  a hypothesis with arithmetic evidence, not a proven cause -- the next step is
  to instrument each candidate's `Open` separately (`PrismEngineBridge.hpp`,
  not this card's file) and to bound or reorder the probe by magic bytes
  (blueprint 7.2 row 2 actually asks for exactly that prefilter).
* **100 000 rows / 120 FPS (Phase 3)**: **CPU-side 125.9 FPS median (7.9 ms/frame,
  p95 15.6 ms)** on this run; the earlier run read 178.2 FPS (5.6 ms, p95 8.0).
  The 8.3 ms budget for 120 Hz is met at the median but the **p95 misses it in
  both runs** (8.0 / 15.6 ms), and this is a SOFTWARE D2D target (a GPU target
  would change this either way), so the cell stays **未达标** with a number
  rather than being rounded up. **Presented FPS is still 未支持**: the window has
  no benchmark injection point (`GalleryWindow` belongs to c1), so
  vsync/composition is not measured. What IS proven is the virtualization: 25
  rows drawn per frame at 1 000 and at 100 000 rows, identically, and the
  per-frame cost does not grow with N. The run-to-run spread (5.6-11.5 ms across
  four runs, including a loaded one) is the machine, not the code.
* **zero-%TEMP% (Phase 3)**: **0 entries before and after a 4 GiB extraction** ->
  **达标**. The mutation that stages the archive through `%TEMP%` turns the cell
  red, so the zero is a guard, not a coincidence of a clean run.

### 4. Not measured, and why (no adjectives, no placeholders)

| Blueprint claim | Status |
|---|---|
| 80 000 x 2 KB small-file write, 42 s -> 6.8 s (IORING + directory pre-create pool) | **未实现，未测**. The IORING writer and the directory pre-create pool do not exist; extraction uses 7-Zip's own output stream. A number here would measure 7-Zip, not us. |
| large pages (`SetLargePageMode`) +11~16% throughput | **未启用，未测**. `7z.dll` exports `SetLargePageMode` and `SevenZipLibrary::EnableLargePages` calls it, but this account holds no `SeLockMemoryPrivilege` (`whoami /priv` lists none; not an administrator), and `MEM_LARGE_PAGES` allocations fail without it -- the export would be a no-op. A prerequisite, not a result. |
| long paths / reserved names, 100% compatible | Not a timing cell: covered by `core.test_windows_name` / `core.test_path_safety`. |
| 4K 120 Hz, 120 FPS | **presented FPS 未支持**; CPU-side 125.9-178.2 FPS median measured (see above; p95 misses the 8.3 ms budget). |
| right-click 12 ms | **不在本卡** (card c2); engine decomposition above. |

### 5. Negative controls (how each guard is broken on purpose)

Every claim ships with a mutant that must fail the SAME assertion. They are
`WILL_FAIL` ctest targets, so "the guard is real" is itself checkable. Local
runs, exit codes as observed:

| Mutant | What it injects | What went red |
|---|---|---|
| `mutation.bench_startup.timer_scale` | every reported duration x 1000 | `Sleep(50) measured 56413.300 ms`, `FAIL ... timer is not calibrated`, exit 1 (re-verified 17:4x on the rebased tree: `54228.800 ms`, exit 1) |
| `mutation.bench_memory.block_buffer` | extract every item into memory and keep it (the naive implementation) | `sweep peak follows block size (219811840 vs 13402112 bytes)`, exit 1 (re-verified 17:4x: `206254080 vs 10592256`, exit 1) |
| `mutation.bench_scale.temp_write` | stage the archive through the controlled `%TEMP%` before extracting | `temp entries 0 -> 1` (`temp: staging.7z`), `FAIL: the pipeline wrote into %TEMP%`, exit 1 (source unchanged by the rebase; not re-run because it re-writes 4 GiB) |

A fourth, unplanned negative control came out of this work and is worth
recording: the first scale run printed its digits and then died with 0xC0000005
**after** `ALL SCALE BENCH CHECKS PASSED`, because `CoUninitialize()` ran before
the last `ComPtr`s were released. The exit code is part of a ctest verdict, so
the suite was red even with the marker present (contract D1's conjunction doing
its job). Fixed with an RAII apartment guard declared first in `main`.

## Report lane (card C42): how the digits reach the run summary

C32 found the hole: `report.yml` ran `ctest --output-on-failure -R "bench"`, and
ctest does **not** print the stdout of a passing test. Every `BENCH {json}` line
from a green bench suite therefore died inside ctest, the summary step printed
"(no BENCH lines)", and the run stayed green. `win.bench_scale`,
`win.bench_memory` and `win.bench_startup` were all affected; only
`win.bench_solid` escaped, because `ci.yml` runs that executable directly.

**What the lane does now.** `ctest -V -R "bench" -E "^(mutation|report)\."`
prints the captured stdout of passing tests, so the BENCH lines reach the log
with ctest's verdict (exit code + `PASS_REGULAR_EXPRESSION` + `TIMEOUT`, contract
D1) still attached. Measured on a compiler-free probe project (2026-11-02, ctest
3.31): plain `ctest --output-on-failure` printed **0** BENCH lines, `ctest -V`
printed **all** of them. Two consequences the parser had to absorb: `-V` prefixes
each captured line with `<n>: `, and ctest prints a test's stdout *before* its
`Test #N: name ... Passed` line -- so attribution keys off `<n>`, never off line
order.

Running each bench executable directly was the other candidate (and is what
`ci.yml` does for `win_bench_solid`): raw stdout, no prefix, but the ctest
verdict is lost -- `PASS_REGULAR_EXPRESSION`, `TIMEOUT` and the WILL_FAIL shape
stop applying, and every suite needs a hardcoded exe name. `-V` was chosen for
that reason, not for taste.

**`tools/report/extract_bench.py` is a gate, not a helper.** It parses
`BENCH` / `BENCH_STARTUP` / `BENCH_MEMORY` / `BENCH_SCALE` /
`BENCH_GLASS_MEM` / `CORE-BENCH` lines into one Markdown table per suite and
appends it to the step summary. The failure modes are red, never silent:

| exit | meaning |
|---|---|
| 0 | every bench line parsed, table written |
| 1 | a malformed bench line (truncated JSON, non-object, `NaN`, `{}`, marker without payload, trailing junk) -- reported with its log line number |
| 2 | no bench lines at all (the C32 bug itself) |
| 3 | the same suite+marker appears twice with different payloads (ambiguous digits) |
| 4 | the log cannot be read |
| 5 | a bench suite is `Passed` but printed no BENCH line (the summary would silently lose it) |

A wrong report is worse than no report: nothing is written to the summary on any
non-zero exit.

**Self-test, no benchmark needed.** `tools/report/test_extract_bench.py` builds
fake `ctest -V` logs in their own temp directory -- good lines for every marker,
one broken line per failure mode, a conflicting duplicate, a silent green suite,
an empty log, a raw executable log -- and asserts exact table rows (counts and
bytes, no stopwatches). Registered as ctest `report.extract_bench` with
`PASS_REGULAR_EXPRESSION "EXTRACT-BENCH SELFTEST: PASSED"`, so a silent skip is
red (D1). Negative control (D3): commenting out the parser's bad-line `raise`
turns the self-test red on its own assertions, and the injection is verified to
have applied before that run -- a replacement that did not match would be a
silent no-op.

**Nightly full gate.** `report.yml` gained `nightly-full`: the complete ctest,
no `-R`/`-E` (bench + fuzz + installer), 90-minute timeout, serial (the win
suites share `%TEMP%` and shell out to `7z.exe`), with the failed test names in
the step summary. The merge gate keeps running the fast subset; this job is the
backstop that runs everything once a night.

Local repro:

```sh
python tools/report/extract_bench.py <ctest -V log> [--summary "$GITHUB_STEP_SUMMARY"]
python tools/report/test_extract_bench.py        # 58 assertions, no bench run
```

## 装配后复测（C49，2026-11-02，`feat/c49-remeasure`）

上面每一节的数字都是**装配前**测的。此后 V1（唯一布局权威）、V2（动作岛）、C29（侧栏）、
C45（缩略图流水线）、C43（嗅探）、C28（路由）先后合入，而它们都会改变用户实际付出的代价。
本节是**装配后**的一次复测：`Tests/win/bench_current.cpp`（ctest `c49.bench_current`），
在同一个进程/同一台机器上把五件事一次测完，每项 ≥5 次采样，给**中位数 + p95**。

**方法（为什么这样测）**

* 首帧、内存都是**真实对象**：`GalleryWindow`（V1 布局 + C29 侧栏 + V2 动作岛 + C33 玻璃岛），
  不是复刻的三岛探针。
* 内存按状态**各起一个全新进程**，由父进程从外部采样工作集（20 次 × 20 ms 的中位数）。
  同一进程里先跑夹具、4 GiB、800 缩略图，工作集就不再是"这个状态"的内存，而是"这个 harness"的内存。
* 4 GiB 那一格是**字节数**（`prism::win::IoCounters`），不是秒表：字节数可复现，秒数在共享 runner 上只测机器。
* 800 缩略图是 C45 流水线的**复跑确认**（串行 vs 并行，各 5 轮）。
* 采样 5 次时 p95 就是五次里的最大值——表里照实写，不假装是统计量。

**机器**：Windows 11 26200，i5-8250U（4 核 8 线程），7-Zip 25.01，VS 2022 17.14 (MSVC 14.44)，Release。
**这台机器是共享的**（七个兄弟工作树在编译），所以同一份二进制跑了三轮：A 轮（安静）、B 轮（有兄弟编译）、
C 轮（ctest 内、report lane 用的那一跑）。三轮都列出来，因为差异不是代码，是机器——
这正是上面 ch.7.2 一节用过的记账方式。

### 1. 冷启动 → 首帧（装配后的真实窗口）

| 采样 | 中位数 | p95 |
|---|---|---|
| A 轮：空作用域 → 首帧画完（CreateWindowExW + 渲染器资源 + 菜单 + 玻璃宿主 + 首次 WM_PAINT） | **311.0 ms** | **441.1 ms** |
| B 轮：同上（带拆分） | **363.8 ms** | **581.2 ms** |
| C 轮：ctest 内（带拆分） | **560.5 ms** | **1143.5 ms** |
| └ 其中 `Create`（窗口 + 设备无关资源 + 菜单） | 91.8 / 199.1 ms（B / C） | — |
| └ 其中 `EnableGlassLayer`（DComp 宿主 + 2 次 HLSL 编译 + 首帧玻璃呈现） | 32.5 / 68.6 ms | — |
| └ 其中**首次 WM_PAINT**（`BeginDraw` + 列表/侧栏/面包屑 + `EndDraw`） | 239.7 / 278.6 ms | 379.2 / 610.1 ms |

**与上一次的差**：旧数字 **22.7 ms** 是"3 个玻璃岛的 BeginDraw+EndDraw+Present+Commit"，
**不含**建窗、不含渲染器的设备资源、不含玻璃宿主的初始化。装配后单是**首次绘制**就是
**204–279 ms**（A 轮总 311.0 − Create/glass ≈ 107 → 绘制 ≈ 204 ms；B/C 轮直接测得 239.7 / 278.6 ms），
整条冷启动路径是 **311–560 ms**。
**是否达标**：ADR-001 的 **首帧 ≤ 60 ms → 未达标**（差 4–6 倍）。
原因不是某一处回归，而是旧数字**从来没有包含**这几段：装配后的首帧必须建窗口、建 D2D/DWrite 资源、
编译两个玻璃效果、再画一整帧 1280×800 的列表 + 侧栏 + 面包屑。**目标数字不改**；
要达标只有两条路：预热的常驻进程（蓝图里有、代码里没有），或把设备相关资源的创建挪出首帧路径。
B/C 轮的 581 / 1143 ms p95 是兄弟编译的负载敏感性，与 C7 记录的 347.7 ms 同性质。

### 2. 常驻内存：四种状态（ADR-006 分档）

| 状态 | 中位数 | p95 | ADR-006 档 | 判定 |
|---|---|---|---|---|
| 空窗口（无归档、无玻璃） | **54.8 MB** | 55.0 MB | —（无档） | 记录 |
| + 1000 条目 catalog（V1 布局 + C29 侧栏，列表模式） | **61.1 MB** | 61.5 MB | — | 记录 |
| + 玻璃岛活体（`EnableGlassLayer` 并呈现） | **63.9 MB** | 63.9 MB | 玻璃全开 ≤ 96 MB | **达标** |
| **出厂 `PrismArchive.exe`**，归档已打开（= 玻璃全开） | **77.5–78.6 MB** | 78.4–79.9 MB | 玻璃全开 ≤ 96 MB | **达标** |
| **出厂 `PrismArchive.exe`**，最小化后 | **73.6–74.0 MB** | 74.7–75.2 MB | 最小化 ≤ 40 MB | **未达标** |

**与上一次的差**：旧的常驻数字是 **58.4 MB**（黑盒、7 条目归档、玻璃 UI 已落地但 C29/C45 未合入）。
现在出厂进程打开归档是 **77.5–78.6 MB**，**+19–20 MB**，方向与 C29 侧栏（树 + 遥测 + 收藏/最近）
和 C45 的缩略图缓存一致；空窗口的地板（**54.8 MB**）说明这 20 MB 不是一次性开销，
而是布局/侧栏/缩略图真的常驻。

**最小化那一档为什么差 34 MB**：C33 的 +40.33 MB 是**满窗**玻璃岛的账（1600×1000，3 pass）；
C34 把岛改成**导航胶囊自己的矩形**（960×48），所以最小化能还给系统的瞬态字节只剩 **3.5–4.0 MB**
（77.5 → 74.0）。剩下的 74 MB 是 D2D/DWrite/字体/渲染器/引擎镜像与 catalog，**按设计不是瞬态的**。
**目标数字不改**：ADR-006 的 40 MB 档是照"满窗玻璃可全还"推的，在当前装配下**够不到**；
要够到，得让渲染器的设备资源也进瞬态（`ReleaseTransient` 之外再加一条路径），那是另一张卡。

### 3. 4 GiB 稀疏 zip / 7z 的打开：**读取字节数**

| 夹具 | 读取字节（中位数 = p95） | 占 4 GiB 的比例 | 打开耗时（中位数） | 路由 |
|---|---|---|---|---|
| `sparse4g.zip` | **263 688 B**（257.5 KiB） | 0.0061 % | 0.3–0.8 ms | `sniff=zip route=sniffed` |
| `sparse4g.7z` | **1 674 B** | 0.00004 % | 0.2–0.3 ms | `sniff=7z route=sniffed` |
| 预算（C28，未改） | ≤ 2 MiB | — | — | — |

**与上一次的差**：旧数字是 **78.5 s**（≈55 MB/s 的整文件扫描；C7 的结论是"疑似候选 handler 顺序"）。
现在同样是 4 GiB 的容器，**读 1.6 KiB–257 KiB 就打开了**，耗时 **0.2–0.8 ms**。
**是否达标**：**达标**（C28 的 2 MiB 字节门限远未触及）。差额的来源就是 C28 的 `FormatSniffer`
（头 64 B + 尾 1 KiB）与 C43 的"magic 即答案，只问一个 handler"。

### 4. 10 万行滚动的帧时间（真实 `GalleryRenderer`，1200×800 软件目标）

| 轮次 | 中位数 | p95 | draw p95 | flush p95 | 画出行数 |
|---|---|---|---|---|---|
| A 轮（5 轮 × 60 帧，取各轮 p95 的中位数） | **4.203 ms** | **6.582 ms** | 4.194 ms | 4.665 ms | 25 |
| B 轮（同二进制，兄弟编译中） | 17.534 ms | 40.339 ms | 25.593 ms | 27.992 ms | 25 |
| C 轮（ctest 内） | 5.874 ms | 11.361 ms | 7.438 ms | 7.842 ms | 25 |

**与上一次的差**：旧数字是 **7.9 ms 中位 / p95 15.6 ms**（C33 自己另记 11.6/20.1）。
A 轮 **4.20 / 6.58 ms** 是**两半都变快**（draw 3.4–4.2 ms，flush 3.5–4.7 ms），
与 V1 把布局收敛成一次计算、C29 侧栏只画命令列表的方向一致。
**是否达标**：120 FPS 的 **8.33 ms** 预算——三轮里**只有 A 轮**（4.20 / p95 6.58 ms）**中位数与 p95 都达标**；
C 轮 p95 11.36 ms、B 轮 p95 40.34 ms 都**未达标**，而三轮的中位数（4.2 / 5.9 / 17.5 ms）有 4 倍差。
所以结论写成**"最安静的机器上达标，正常/满载时未达标"**，而不是把 A 轮当成保证。
虚拟化仍然精确：1 千行与 10 万行都只画 **25 行**。

### 5. 800 张缩略图：串行 vs 并行（C45 流水线复跑）

| 轮次 | 串行中位数 | 并行中位数 | 加速比 | worker | 像素 |
|---|---|---|---|---|---|
| A 轮（各 5 轮） | **80.7 ms**（p95 83.5） | **60.3 ms**（p95 63.4） | **1.34×** | 4 | 逐字节相同 |
| B 轮 | 106.0 ms（p95 241.3） | 74.8 ms（p95 101.9） | 1.42× | 4 | 逐字节相同 |
| C 轮（ctest 内） | 104.2 ms（p95 158.1） | 68.2 ms（p95 78.4） | **1.53×** | 4 | 逐字节相同 |

**与上一次的差**：C45 的两臂在**装配后**复跑，形状不变，但加速比只有 **1.34–1.53×**（4 worker）：
这一格的瓶颈不在解码——800 次 128×128 合成解码本身只要 80–106 ms，余下的是 `PostMessage`
交接 + UI 线程逐条 apply。**是否达标**：这一格**没有目标数字**（C45 的契约是"每条只解码一次、
第二次 100 % 命中、像素逐字节相同、常驻不超 64 MiB"），四条**全部达标**
（`decoded=800`、`second_pass_hits=800`、两轮 `pixel_digest` 相同、`resident=50 MiB ≤ 64 MiB`）。

### 6. 与 ADR-006 分档的对照（一句话结论）

| ADR-006 条目 | 预算 | 实测（装配后） | 判定 |
|---|---|---|---|
| 玻璃全开（出厂进程，归档打开） | ≤ 96 MB | **77.5–78.6 MB** | **达标**（余量 17–19 MB） |
| 最小化 / 无岛可见 | ≤ 40 MB | **73.6–74.0 MB** | **未达标**：只还回 3.5–4.0 MB，因为岛是 960×48（C34）而 74 MB 的主体不是瞬态 |
| 缩略图缓存（独立预算） | ≤ 64 MiB | **50 MiB**（800 × 128² 满载） | **达标**（D17 的逐字节容量契约由 `core.test_thumb_budget` 守） |
| 首帧（ADR-001，未改） | ≤ 60 ms | **311–560 ms** | **未达标**（见 §1 的拆分） |
| 120 FPS（未改） | ≤ 8.33 ms | **4.20 / p95 6.58**（A）；5.87 / 11.36（C）；17.5 / 40.3（B） | **仅安静轮达标** |

### 7. BENCH 行与负向控制（D27）

每次运行打印五条机器可读行，全部走 C42 的 report lane（`tools/report/extract_bench.py` 实测解析
exit 0，五个 marker 各出一张表）：

```text
BENCH_C49_STARTUP {"first_frame_median_ms":..,"first_frame_p95_ms":..,"create_median_ms":..,
                   "glass_median_ms":..,"paint_median_ms":..,"paint_p95_ms":..,"samples":9,...}
BENCH_C49_MEMORY  {"empty_mb":..,"archive_mb":..,"glass_full_mb":..,
                   "app_archive_mb":..,"app_minimized_mb":..,"adr006_glass_budget_mb":96,...}
BENCH_C49_OPEN4G  {"zip_bytes":..,"sevenz_bytes":..,"budget_bytes":2097152,"file_bytes":4294967296,...}
BENCH_C49_FRAME   {"rows":100000,"median_ms":..,"p95_ms":..,"draw_p95_ms":..,"flush_p95_ms":..,"rows_drawn":25,...}
BENCH_C49_THUMBS  {"items":800,"serial_ms":..,"parallel_ms":..,"speedup":..,"workers":4,"pixel_digest":"..",...}
```

**负向控制** `mutation.c49.no_sniff`（CONTRACTS D3 + D26 + **D27**）：把 C28 的嗅探扔掉
（`PRISM_C28_MUTANT_PROBE_ALL`），4 GiB 的字节门必须红。**第一次尝试是假的**：
诚实扩展名的 `.zip`/`.7z` 即使不嗅探也会走扩展名路由，字节数仍是 263 688，
`mutantProbeAll=2`（注入确实发生）而门是绿的——记下来，因为这是"注入发生了但断言没红"，
不能算捕获。换成 C28 的锋利夹具（**一个 4 GiB 的 7z 戴 `.tar` 名字**，tar 没有索引，
扩展名路由只能走完整个文件）后：

```text
sparse4g-lie.tar: bytes=4296018656
FAIL  Tests/win/bench_current.cpp:1082  expected true: sample.bytes <= kOpenByteBudget
C49 MUTATION CAUGHT: no_sniff (first failure at Tests/win/bench_current.cpp:1082; exiting 0 now so teardown cannot swallow the verdict)
```

裁决在**首红那一刻**打印、进程 `_Exit(0)`，ctest 的 `PASS_REGULAR_EXPRESSION`
（`C49 MUTATION CAUGHT: no_sniff`）就是判定；没有 `WILL_FAIL`（D26）。
谓词是 `IoCounters().mutantProbeAll > 0`，所以因无关原因变红不会谎报 CAUGHT（D3）。

**复现**：

```powershell
. E:\wg\env.ps1 -Card c49
cmake --build E:\build\wt-c49 --target c49_bench_current PrismArchive
ctest --test-dir E:\build\wt-c49 -R "c49\.bench_current|mutation\.c49" -V
```

## 装配后复测（C65，2026-11-03，feat/c65-perf）

C49 的复测停在 2026-11-02：之后标签栏（C59/C60）、搜索框（C62）、编辑 UI（C54）、
诊断窗口（C57）、保险库（C56）才陆续合入出厂二进制。它们都改变用户实际付出的代价，
而 **C49 之后没有任何一次复测**。本节是同一台机器上的第二次装配后复测，只改测试与文档，
产品代码一行未动。

### 0. 方法与采样点（哪条命令、哪个进程、什么时刻）

| 项 | 进程 / 命令 | 采样时刻 |
|---|---|---|
| 构建 | . E:\wg\env.ps1 -Card c65 → Enter-BuildLock → cmake --build E:\build\wt-c65 --target PrismArchive c65_bench_current perf_smoke ... --parallel 2 | 持构建锁（本次 slot2），不绕过 |
| 启动→首帧（进程内） | ctest --test-dir E:\build\wt-c65 -R "^c65\.bench_remeasure$" -V → c65_bench_current --c65 | 每轮 9 个真实 GalleryWindow：Create + EnableGlassLayer + 首次 WM_PAINT（UpdateWindow 同步返回） |
| 进程启动→主窗口（黑盒） | Start-Process PrismArchive.exe <archive>，MainWindowHandle 每 5 ms 轮询 | 3 次全新进程；数的是 CreateProcess → 顶层窗口句柄出现（**不含**首帧绘制） |
| 4 GiB 打开 / 首字节 | 同上，MeasureOpen：IoCounters().bytesRead + Open() + ExtractToMemory(0) | 每个夹具 5 次；字节数可复现，毫秒只作参考 |
| 10k / 100k 行帧 | 同上，真实 GalleryRenderer，1200×800 **软件**目标 | 每格 5 轮 × 60 帧；报每轮中位 / p95，再取 5 轮中位 |
| 800 张缩略图 | 同上，C45 ThumbnailPipeline，128×128，4 worker | 串行 / 并行各 5 轮 |
| 常驻内存 | 每状态起一个**全新子进程**；父进程用 GetProcessMemoryInfo 读 **WorkingSetSize** 与 **PagefileUsage**（两次独立测量） | 子进程 settle 后父进程连读 20 次 × 20 ms 取中位；每状态 3 个全新进程，再取 3 个中位。开归档：主窗口出现后 2500 ms；最小化：ShowWindow(SW_MINIMIZE) 后 1500 ms |

内存“两次独立测量”的含义：同一批 20 次读取里同时取两个**不同语义**的计数器——
Working Set（当前驻留页，可被 Windows 随时 trim）与 Private Bytes（私有提交，trim 不归还）。
一个状态只有**两个都记录**才算测过；本卡不复用任何单计数器结论。

**机器与负载。** Windows 11 26200，i5-8250U（4 核 8 线程），7-Zip 25.01，VS 2022 17.14
（MSVC 14.44），Release。机器是共享的（七个兄弟工作树在编译），所以 C65 跑了 **5 轮**：R1/R4 落在高负载窗口（同一轮里 draw 与 flush 同时膨胀），R2/R3 与全量 bench lane 内的 R5 落在低负载窗口。五轮都列出；判定用低负载轮，高负载轮作为敏感性证据。
另跑 4 次 C49 默认 lane（c49.bench_current）作对照，证明两个 lane 的差异随机器翻转。

### 1. 启动 → 首帧

| 轮次 | 首帧中位 | p95 | Create 中位 | Glass 中位 | 首次绘制中位 |
|---|---|---|---|---|---|
| R1（高负载） | 188.4 ms | 264.3 ms | 54.0 ms | 18.6 ms | 116.1 ms |
| R2 | 180.0 ms | 232.1 ms | 47.6 ms | 16.6 ms | 90.2 ms |
| R3 | 162.9 ms | 208.4 ms | 46.8 ms | 17.3 ms | 96.4 ms |
| R4（高负载） | 161.3 ms | 188.9 ms | 47.0 ms | 17.0 ms | 90.7 ms |
| R5（全量 bench lane） | 164.5 ms | 183.2 ms | 48.9 ms | 16.6 ms | 96.9 ms |
| **五轮中位** | **164.5 ms** | **208.4 ms** | 47.6 ms | 17.1 ms | 94.2 ms |

黑盒（出厂 PrismArchive.exe，3 次全新进程）：**191.9 / 193.5 / 294.7 ms**，
中位 **193.5 ms**（CreateProcess → 主窗口句柄）。这是“窗口已建立”，不是“首帧已画完”；
把进程内的首次绘制（≈93 ms）加上去，端到端首帧约 **287 ms**——这个加法是估值，不是一次采样。

**与旧值的差**：C49 A 轮 **311.0 / 441.1 ms** → 现在五轮中位 **164.5 / 208.4 ms**
（−47.1 % / −52.8 %）。**这不是产品变快**：用同一个 C49 默认 lane 在本次安静窗口复跑，
首帧是 **204.1 / 256.0 ms**；负载轮 R1 是 188.4 / 264.3 ms。311 ms 是 C49 当时机器状态的读数
（同节已记录“兄弟编译下 347.7 ms”），不是 C49 代码的常数。
**是否达标**：ADR-001 的 **首帧 ≤ 60 ms → 未达标**（仍差 2.7–3.5 倍）。原因与 C49 相同：
窗口 + D2D/DWrite 设备资源 + 两次 HLSL 编译 + 一整帧 1280×800 列表/侧栏/面包屑
都在首帧路径上；要达标得靠预热的常驻进程或把设备资源挪出首帧，二者都还没有。

### 2. 4 GiB 归档：读取字节与首字节

| 夹具 | 读取字节（5 次恒定） | Open 中位 | Open → 首载荷字节 中位 | C28 门限 |
|---|---|---|---|---|
| sparse4g.zip | **263 688 B**（257.5 KiB，0.0061 %） | 0.24–0.56 ms | **0.27–0.63 ms** | ≤ 2 MiB **达标** |
| sparse4g.7z | **1 674 B**（0.00004 %） | 0.14–0.36 ms | **0.20–0.50 ms** | ≤ 2 MiB **达标** |

“首字节”是两次调用，不是一个：Open() 只保证目录可读，第一条载荷要 ExtractToMemory(0)。
夹具里的载荷是 22 B 的 stored item，所以这个返回时刻与首字节的差在微秒级；
first_payload_read=true 是 5 次都真的读到了，不是推断。旧值（C49）**263 688 B / 1 674 B、
打开 0.2–0.8 ms**，字节数逐字节一致，**达标**。旧 §3 的 78.5 s 是 C28 之前的整文件扫描，
已被 C28/C43 的嗅探路径替代，本次没有回归。

### 3. 帧时间：10k 与 100k（同一渲染器）

| 轮次 | 10k 中位 / p95 | 100k 中位 / p95 | 画出行数（10k / 100k） |
|---|---|---|---|
| R1（高负载） | 13.103 / 31.917 ms | 16.828 / 32.262 ms | 25 / 25 |
| R2 | 4.070 / 5.779 ms | 4.181 / 6.277 ms | 25 / 25 |
| R3 | 4.150 / 6.176 ms | 5.114 / 6.519 ms | 25 / 25 |
| R4（高负载） | 10.799 / 25.748 ms | 13.334 / 24.309 ms | 25 / 25 |
| R5（全量 bench lane） | 3.495 / 5.843 ms | 4.238 / 6.177 ms | 25 / 25 |
| **低负载三轮（R2/R3/R5）** | **3.50–4.15 / 5.78–6.18 ms** | **4.18–5.11 / 6.18–6.52 ms** | 25 / 25 |

**与旧值的差**：旧 100k 是 C7 的 7.9 / p95 15.6 ms，C49 A 轮 4.203 / p95 6.582 ms。
本次 100k p95 **6.18–6.52 ms**，对 C49 A 是 −1.0 %～−6.2 %；10k 没有旧值，本次首测
**5.78–6.18 ms**。10k 与 100k 的代价相同、都只画 **25 行**，虚拟化仍然精确。
**是否达标**：120 FPS 的 8.33 ms 预算——低负载三轮**中位与 p95 都达标**；
R1/R4 的 p95 24.3–34.5 ms **未达标**。把 R1/R4 当回归会是错的：同一轮里 draw 与 flush
**同时**膨胀（flush 是 D2D 光栅化，产品 UI 代码根本不碰），且交错跑 C49/C65 时方向会翻转：
pair 1 里 C65 的 100k p95（12.2）**低于** C49（14.6），pair 2 里反过来（28.4 vs 6.3）。
所以这一格的结论只能是“安静机器上达标，满载时未达标”，与 C49 的写法一致。

### 4. 800 张缩略图：串行 vs 并行（C45 流水线复跑）

| 轮次 | 串行中位 / p95 | 并行中位 / p95 | 加速比 | 吞吐（并行） |
|---|---|---|---|---|
| R1（高负载） | 159.9 / 216.7 ms | 105.7 / 130.2 ms | 1.51× | 7 566 张/s |
| R2 | 75.9 / 90.4 ms | 53.7 / 59.6 ms | 1.41× | 14 903 张/s |
| R3 | 77.0 / 86.8 ms | 56.2 / 61.0 ms | 1.37× | 14 244 张/s |
| R4（高负载） | 165.7 / 250.3 ms | 114.4 / 133.7 ms | 1.45× | 6 992 张/s |
| R5（全量 bench lane） | 73.9 / 88.1 ms | 54.9 / 57.1 ms | 1.35× | 14 576 张/s |

旧值（C49 A）串行 **80.7 ms**、并行 **60.3 ms**、**1.34×**；本次低负载三轮
串行 **73.9–77.0 ms**（−4.6 %～−8.4 %）、并行 **53.7–56.2 ms**（−6.8 %～−10.9 %）、
**1.35–1.41×**。契约四项全部成立：decoded=800、second_pass_hits=800、
串行/并行的 pixel_digest 都是 **fb62252085a20b13**、常驻 **52 428 800 B = 50 MiB ≤ 64 MiB**。
这一格没有目标数字，**判定：达标（且比旧值略快）**。

### 5. 常驻内存：Working Set 与 Private Bytes（每状态 3 个全新进程）

| 状态 | Working Set（3 次原始值 → 中位） | Private Bytes（3 次原始值 → 中位） | 旧值（C49，仅 WS） |
|---|---|---|---|
| 空窗口（无归档、无玻璃） | 46.11 / 46.13 / 46.18 → **46.13 MB** | 16.54 / 16.65 / 16.80 → **16.65 MB** | 54.8 MB |
| + 1000 条目 catalog | 47.05 / 47.12 / 47.25 → **47.12 MB** | 17.62 / 17.68 / 17.70 → **17.68 MB** | 61.1 MB |
| + 玻璃岛活体 | 49.43 / 49.47 / 49.43 → **49.43 MB** | 19.88 / 19.95 / 20.10 → **19.95 MB** | 63.9 MB |
| **出厂 exe，归档已打开** | 74.77 / 75.66 / 76.58 → **75.66 MB** | 47.93 / 49.20 / 50.08 → **49.20 MB** | 77.5–78.6 MB |
| **出厂 exe，最小化后** | 71.24 / 71.50 / 71.70 → **71.50 MB** | 45.30 / 45.19 / 45.20 → **45.20 MB** | 73.6–74.0 MB |

五个 C65 轮次的出厂值（WS / PB，MB）：R1 74.77/47.93、R2 75.66/49.20、R3 76.58/50.08、
R4 75.95/49.52、R5 76.46/49.86；最小化 R1 71.24/45.30、R2 71.50/45.19、R3 71.70/45.20、
R4 71.29/45.46、R5 71.55/45.23。同一状态五轮离散度：WS ≤ 2.4 %、PB ≤ 4.5 %，比帧时间稳定得多——这正是要把 Private Bytes 一起记的原因。

**与旧值的差**：开归档 WS **−2.4 %**（旧中位 78.0 → 76.1 MB），最小化 WS **−3.0 %**（73.8 → 71.6 MB），
都在噪声带内，**没有回归**。空窗口旧 54.8 MB → 46.1 MB（−15.8 %）也不是产品瘦身：
同一个 C49 默认 lane 在本次安静窗口复跑，空窗口同样是 **46.1 MB**、开归档 **75.4 MB**、
最小化 **69.3 MB**（p95 72.8）。54.8 是旧机器状态下的 WS 读数，WS 本来就会被 trim/负载改变；
Private Bytes 才是更稳的基线，本次首次建立（空 16.7 / 开归档 49.2 / 最小化 45.2 MB）。

**最小化只还回多少**：开归档 → 最小化，WS 只降 **4.2–4.9 MB**，PB 只降 **4.0–4.9 MB**。
这与 C34 把玻璃岛缩到导航胶囊（960×48）之后的账一致：能还的是瞬态玻璃，不能还的是
D2D/DWrite/字体/渲染器/引擎镜像/catalog。**按设计不是泄漏**，但也够不到 ADR-006 的 40 MB。

### 6. 旧值 → 新值 → 判定（逐项）

“新值”取 C65 低负载轮 R2/R3/R5；判定同时给出 ADR-006/ADR-001 的目标对照。

| # | 指标 | 旧值 | 新值（C65） | 变化 | 判定 |
|---|---|---|---|---|---|
| 1 | 启动→首帧（进程内，中位/p95） | 311.0 / 441.1 ms（C49 A） | 164.5 / 208.4 ms（五轮中位；低负载 162.9–180.0 / 183.2–232.1） | −47.1 % / −52.8 % | **未达标**（目标 60 ms，仍 2.7×）；相对旧值为**改善** |
| 2 | 进程启动→主窗口（黑盒） | 800.8 ms（C7，满负载；空闲 497 ms） | 193.5 ms（3 次：191.9/193.5/294.7） | — | 记录；较旧值为改善，**无 60 ms 档** |
| 3 | 4 GiB zip：读取字节 / 首字节 | 263 688 B / 0.3–0.8 ms | 263 688 B / 0.27–0.63 ms | 0 % / ≈0 | **达标**（C28 2 MiB 门限） |
| 4 | 4 GiB 7z：读取字节 / 首字节 | 1 674 B / 0.2–0.3 ms | 1 674 B / 0.20–0.50 ms | 0 % / ≈0 | **达标** |
| 5 | 10k 行帧 p95 | 无旧值 | 5.78–6.18 ms | — | **达标**（8.33 ms） |
| 6 | 100k 行帧 p95 | 6.582 ms（C49 A）；15.6 ms（C7） | 6.18–6.52 ms | −1.0 %～−6.2 % / −58 %～−60 % | **达标**（低负载） |
| 7 | 缩略图串行 | 80.7 ms | 73.9–77.0 ms | −4.6 %～−8.4 % | **达标**（无目标；契约全过） |
| 8 | 缩略图并行 | 60.3 ms | 53.7–56.2 ms | −6.8 %～−10.9 % | **达标** |
| 9 | 缩略图加速比 | 1.34× | 1.35–1.41× | +0.7 %～+5.2 % | **达标** |
| 10 | 缩略图常驻 | 50 MiB | 50 MiB（52 428 800 B） | 0 % | **达标**（≤ 64 MiB） |
| 11 | 开归档 WS | 77.5–78.6 MB | 75.7–76.6 MB | −2.4 % | **达标**（≤ 96 MB） |
| 12 | 开归档 PB | 无旧值 | 49.2–50.1 MB | 新基线 | **达标**（≤ 96 MB） |
| 13 | 最小化 WS | 73.6–74.0 MB | 71.5–71.7 MB | −3.0 % | **未达标**（≤ 40 MB，旧值也未达标） |
| 14 | 最小化 PB | 无旧值 | 45.2 MB | 新基线 | **未达标**（≤ 40 MB） |
| 15 | 空窗口 WS | 54.8 MB | 46.1 MB | −15.9 % | 记录（非回归；同 build 的 C49 lane 同样 46.1） |

**没有任何一项相对 docs/PERF.md 的旧值回归超过 10 %。** 因此本卡没有改产品代码，
也没有开回归 issue。超过 10 % 的移动只有两类：空窗口 WS 的 −15.8 %（更低，是机器状态），
以及 R1/R4 负载轮的帧时间（同一轮 draw 与 flush 同时膨胀，交错跑会翻转方向）。

**对“半个月新增 UI 是否拖垮内存/启动”的回答（带数字）**：没有。
出厂进程开归档 75.7–76.6 MB WS / 49.2–50.1 MB PB，相对 C49 的 77.5–78.6 MB 反而低 2–3 %；
最小化 71.5–71.7 MB，低 3 %；首帧 162.9–180.0 ms，不比 C49 的 204.1 ms（同 lane 复跑）慢。
标签栏/搜索框/编辑 UI/诊断/保险库的状态没有以常驻内存的形式留下来（或至少被
Private Bytes 的 49.2 MB 上界盖住）。**代价仍然是 ADR-006 早已记录的那一项：最小化 40 MB 够不到。**

### 7. ADR-006 与 ADR-001 门槛逐条对照

| 条目 | 预算 | C65 实测（WS / PB） | 判定 |
|---|---|---|---|
| 玻璃全开（出厂进程，归档打开） | ≤ 96 MB | **75.7–76.6 / 49.2–50.1 MB** | **达标**（WS 余量 19.4 MB，PB 余量 45.9 MB） |
| 最小化 / 无岛可见 | ≤ 40 MB | **71.5–71.7 / 45.2 MB** | **未达标**：WS 超 31.5–31.7 MB、PB 超 5.2 MB；最小化只还 4.2–4.9 MB |
| 缩略图缓存 | ≤ 64 MiB | **50 MiB**（52 428 800 B） | **达标**（D17 容量契约由 core.test_thumb_budget 守） |
| 硬上限：预算放宽 ≠ 无界 | 任何 op 后 resident ≤ budget | 800 张满载仍 50 MiB；1000 次随机 op 0 次越界（既有 core.bench_sweep） | **达标** |
| 首帧（ADR-001，未改） | ≤ 60 ms | **164.5 / 208.4 ms**（五轮中位） | **未达标** |
| 120 FPS（未改） | ≤ 8.33 ms | 低负载 **3.50–4.15 / 5.78–6.18 ms**；高负载 **10.8–13.1 / 24.3–34.5 ms** | **安静达标，满载未达标**（与 C49 同性质） |

### 8. 每次合并都能跑的 3 秒冒烟（perf_smoke）

新增 ctest perf_smoke（Tests/suites/c65-perf.cmake，源码 Tests/win/bench_current.cpp --smoke）：
一个真实 GalleryWindow 到首次绘制 + 60 帧 10k 行列表 + 两个内存计数器的可读性检查。
断言只有形状（painted、画出 25 行、p95 < 5 s 的挂死检测），不门禁秒表（D1/D29）。

   276: BENCH_C65_SMOKE {"elapsed_ms":442.7,"first_frame_ms":128.6,"frame_median_ms":4.000,
                         "frame_p95_ms":8.177,"rows":10000,"rows_drawn":25,
                         "ws_mb":33.03,"private_mb":14.36}
   276: ALL PERF SMOKE CHECKS PASSED
   276: Passed 0.49 sec

同一二进制在负载窗口也跑过：elapsed_ms=2768.7、ctest 3.36 s——仍在 3 秒量级；
TIMEOUT 15 是挂死预算，不是期望时长。负向控制 mutation.c65.no_first_paint
（D3 + D26 + D27，无 WILL_FAIL，marker 即合取）：

   277: C65 MUTANT ACTIVE: no_first_paint
   277:     FAIL  Tests/win/bench_current.cpp:1044  expected true: first.painted
   277: C65 MUTATION CAUGHT: no_first_paint (first failure at ...; exiting 0 now so teardown cannot swallow the verdict)
   277: Passed 0.96 sec

谓词是注入点实际写下的 gNoFirstPaintFired，不是“程序名带 mutant”——因无关原因变红
不会打印 CAUGHT。

### 9. C75：最小化内存分解与回落（2026-11-03，feat/c75-trim）

C65 的 71.5–71.7 MB 不是猜的，是每次最小化只还 4.2–4.9 MB。C75 先做分解，再按
“最大且可安全释放”的顺序改。所有对象数字来自活 COM 指针/容器大小（C33 的会计规则），
进程数字来自 `GetProcessMemoryInfo`；测试是 `Tests/win/c75_mem_idle.cpp`（真实
`GalleryWindow`、真实 D2D hwnd target、DWrite、玻璃岛、64 张 320×240 缩略图 = 18.75 MB）。

#### 9.1 最小化前/后的分解（同一进程，`C75_BREAKDOWN` 两行）

| 类别 | 最小化前 | 最小化后 | 说明 |
|---|---:|---:|---|
| 缩略图像素（ThumbStore） | **18.75 MB**（64 项，64 MiB 预算的 29 %） | **0** | C34 只裁到预算的 1/8（8 MiB），C75 改为 `ReleaseAll` |
| D2D hwnd target（后台缓冲 1100×740×4） | **3.57 MB** | **0**（target 本身销毁） | 同时带走它内部 D3D 设备的驱动缓存 |
| 玻璃瞬态（交换链 2×960×48 + 效果图） | **0.35 MB** | **0** | 原有 `ReleaseTransient` 路径 |
| 渲染器设备对象 | 14 画刷 + 11 文本格式 | **0 + 0** | 新增 `ReleaseDeviceResources`；含 tab/action 画刷与 DWrite 格式 |
| DWrite 工厂 / 字体引用 | 2 个工厂 + 1 crumb 格式 | 0 | 随 target 一起释放，恢复时重建 |
| catalog + 行模型 | 800 项 / 15 890 B 名字 + 1 行视图 | 不变 | 小且恢复要用；不释放 |
| 其它（进程堆、引擎、映射 DLL） | 余量 | 余量 | 不可由窗口释放（见 9.3） |

进程总数（同一测试进程）：**最小化前 66.6 MB WS / 41.8 MB PB → 最小化后 44.0 MB WS /
19.1 MB PB**；**实际还回 22.1 MB WS / 22.6 MB PB**，是 C65 那 4.2–4.9 MB 的约 4.6 倍。

出厂 `PrismArchive.exe`（64 张真实 PNG 的 7z，含缩略图扫描）外部采样：
**开归档 71.4 MB WS / 43.3 MB PB → 最小化 60.7 MB WS / 33.0 MB PB**（两轮 60.71/60.94）。
同一 exe 用“全是坏 PNG、解码不出缩略图”的归档量到的**地板**是 49.8 MB WS / 22.3 MB PB：
这 10.9 MB 差就是“解码过 18.75 MB 缩略图后，堆把已释放的页留在提交里”的部分，不是仍在
被引用的对象（`C75_BREAKDOWN` 的 thumb_bytes 已是 0）。

#### 9.2 改了什么（产品）

- `GalleryWindow::ReleaseRenderMemory()`：最小化路径一次性释放玻璃瞬态、**全部**缩略图、
  渲染器 target 绑定对象、窗口的 crumb 格式/DWrite 引用，最后销毁 `ID2D1HwndRenderTarget`
  并调用 `IDXGIDevice3::Trim()`（有玻璃时走 `GlassIslandHost::TrimDevice()`）。
- `GalleryWindow::RestoreRenderMemory()`：在 **WM_SIZE(SIZE_RESTORED) 内同步**
  重建 target + 设备对象 + 玻璃瞬态，并立刻 `Render()` 一帧；不把首帧留给下一次
  `WM_PAINT`（空白帧的成因）。
- `GalleryRenderer::ReleaseDeviceResources()` 与两个 painter 的同类方法：从活指针推导
  画刷/格式/缩略图设备位图的数量与字节。
- 恢复代价：五次循环的“重建+首帧”为 **8–28 ms**（中位约 10–16 ms），与 C65 的一次
  完整首帧 paint（中位 92.2 ms）同量级或更低；循环内断言 ≤ 第一次的 1.2× + 2 ms
  （QPC 计时；本机调度抖动可见 8–28 ms 的离散，20 % 是合同线，2 ms 是计时粒度）。

#### 9.3 40 MB 没达到：差在哪，为什么

最小化后仍有 **44.0 MB**（测试窗口）/ **60.7 MB**（出厂进程）。把窗口侧能释放的**全部**
释放后，进程地板仍是 44 MB 左右：这部分是
D3D11/D2D/DWrite/Windows.Storage/igd 驱动等**已映射的 DLL 代码页**（约 2.4 MB DWrite、
6.2 MB d2d1、2.4 MB d3d11、8.7 MB Windows.Storage、以及 Intel 驱动的 igc64/igd10iumd
映射）和进程堆的常驻页。它们不在任何 COM 对象上，`ReleaseTransient`/`Trim` 也够不到。

出厂进程比测试窗口多的约 16.7 MB 是**应用层**持有的：7z.dll 引擎与 reader pool、
缩略图解码后的堆保留页（上面那 10.9 MB）以及设置/会话/语言表。要让 60.7 → 40，
必须（a）在最小化时 park/关闭 reader pool 与解码管线（下一张卡，产品生命周期改动），
以及（b）让堆把已释放的 10 MB 真正还给系统（`HeapCompact`/空闲态清理，需单独验证
恢复代价）。本卡**没有**用 `SetProcessWorkingSetSize(-1,-1)`/`EmptyWorkingSet` 把
数字做成绿色：那是把代码页换出，恢复首帧会付页错误，和“≤ 原恢复时间 + 20 %”冲突，
也不是 ADR-006 写的“靠 ReleaseTransient + Trim 回落”。

因此本卡交付的是：**分解表 + 22 MB 的真实回落 + 恢复不空白 + 两条负向控制**；
ADR-006 的 40 MB 档仍**未达标**（测试打印 `C75_ADR006 {"met":false}`，不伪造绿色）。

#### 9.4 复现

    . E:\wg\env.ps1 -Card c75
    cmake --build E:\build\wt-c75 --target c75_mem_idle c75_mem_idle_mutant_no_trim c75_mem_idle_mutant_no_restore_rebuild PrismArchive
    ctest --test-dir E:\build\wt-c75 -R "c75|mutation\.c75" -V

负向控制（D3/D26/D27，无 WILL_FAIL）：

- `mutation.c75.no_trim`：最小化路径不释放 → `!window.TargetLive()` 第一条红，
  打印 `C75 MUTATION CAUGHT: no_trim`（实测最小化 66.0 MB，与开归档 66.0 MB 持平）。
- `mutation.c75.no_restore_rebuild`：恢复的 Resize 不重建不重画 → 首帧像素
  `non_backdrop=0.0000`，空白帧断言红，打印 `C75 MUTATION CAUGHT: no_restore_rebuild`。

**ASan lane 的判据（CI 实测教训）**：Windows CI 用 `/fsanitize=address` 构建，ASan 的
quarantine 会把已释放的分配留着不还给 OS——同一轮测试里对象已证死（target/thumb/glass
全为 0），WS 却是 112.90 → 112.73 MB（只回 0.17 MB），而且每次 restore 再涨约 4.8 MB。
所以字节判据**不能**写死在 ASan lane 上：`c75_mem_idle` 检测到 ASan 时只断言**行为**
（对象已释放、target 不再存活、首帧非空白），字节数只打印；非 ASan 构建用**相对判据**
（回落量 ≥ 最小化前 WS 的 15 %，本机实测约 30 %）。轮次增长在 ASan 上同样不判，
非 ASan 改判“第 N 轮 ≤ 第 1 轮 + 2 MB”（不是逐轮 +1 MB，避免一轮噪声把预算抬高）。
恢复预算用后续轮次的**中位数** ≤ 首次 × 1.2 + 10 ms（10 ms 是本机计时/调度粒度；
完整重建设备是 60–100 ms，仍会红）。

C34 的守卫也重新接上：`ReleaseRenderMemory` 现在经 `ReleaseGlassLayer()` 释放玻璃
（该函数才是 `PRISM_C34_MUTANT_NO_TRIM` 的注入点），所以 `mutation.host_wiring.no-trim`
重新在 `GlassTransientBytes()==0` 处变红；C52 `win.drop_in` 的“一次失效一次绘制”契约
也不再被多余同步帧破坏（只有 `idleReleased_` 为真、即最小化路径确实跑过时才在
WM_SIZE 里同步重建+画一帧）。

#### 9.5 其余待办（原 C65 §9 的第 2–3 项）

1. **帧/缩略图数字的负载噪声**：同一二进制在 90 分钟内读到 10k p95 5.78–34.46 ms、
   串行缩略图 75.9–177.3 ms；交错跑 C49/C65 两次，pair 1 里 C65 更快、pair 2 里更慢。
   这说明差值主要来自机器而不是 lane，但**没有**排除“C65 先跑 10k 再跑 100k”的顺序效应。
   最小复现（三次交替，比较同窗口内的 p95）：

       for ($i in 1..3) {
         ctest --test-dir E:\build\wt-c65 -R "^c49\.bench_current$" -V | Select-String 'BENCH_C49_FRAME'
         ctest --test-dir E:\build\wt-c65 -R "^c65\.bench_remeasure$" -V | Select-String 'BENCH_C65_FRAME'
       }

   若同窗口内 C65 的 p95 稳定高出 >10 %，再查 RunFrameRound 的复用/顺序（这是测试代码，不动产品）。

2. **没有 >10 % 回归项**，所以没有“回归 issue”；产品代码、docs/PROGRESS.md、
   ROADMAP.md、CONTRACTS.md、ci.yml、Tests/CMakeLists.txt 均未改。

### 10. 复现（C65 全套）

    . E:\wg\env.ps1 -Card c65
    cmake --build E:\build\wt-c65 --target c65_bench_current perf_smoke perf_smoke_mutant_no_first_paint PrismArchive
    ctest --test-dir E:\build\wt-c65 -R "^c65\.bench_remeasure$" -V        # ~47–58 s，5 条 BENCH_C65_* 行
    ctest --test-dir E:\build\wt-c65 -R "perf_smoke|mutation\.c65" -V      # ~1.5 s（安静）

C65 的 BENCH 行（每轮一行，C42 report lane 可直接解析）：

    BENCH_C65_STARTUP   {"mode":"C65","first_frame_median_ms":..,"first_frame_p95_ms":..,"samples":9,...}
    BENCH_C65_MEMORY    {"empty_mb":..,"empty_private_mb":..,"archive_mb":..,"archive_private_mb":..,
                         "glass_full_mb":..,"glass_full_private_mb":..,"app_archive_mb":..,
                         "app_archive_private_mb":..,"app_minimized_mb":..,"app_minimized_private_mb":..,
                         "samples":3,"adr006_glass_budget_mb":96,"adr006_min_budget_mb":40}
    BENCH_C65_OPEN4G    {"zip_bytes":..,"zip_first_byte_ms":..,"sevenz_bytes":..,"sevenz_first_byte_ms":..,
                         "first_payload_read":true,"budget_bytes":2097152,"file_bytes":4294967296,...}
    BENCH_C65_FRAME     {"rows":10000,"median_ms":..,"p95_ms":..,"draw_p95_ms":..,"flush_p95_ms":..,
                         "rows_drawn":25,...}
    BENCH_C65_FRAME_100K {"rows":100000,...}
    BENCH_C65_THUMBS    {"items":800,"serial_ms":..,"parallel_ms":..,"speedup":..,"items_per_sec":..,
                         "decoded":800,"second_pass_hits":800,"resident_bytes":52428800,"pixel_digest":".."}

### 11. 全量 `ctest -R bench`（12 项）与第 5 轮

`ctest --test-dir E:\build\wt-c65 -R "bench" --output-on-failure -V` 把 bench 家族一次跑完。
本次跑到宿主的 16 分钟后台作业上限时，**11/12 已通过**（core.bench_sweep、win.bench_solid、
c45.bench_thumbs、report.extract_bench、c49.bench_current、c65.bench_remeasure、
win.bench_startup + 其变异、win.bench_memory + 其变异、win.bench_scale）；被截断的是**既有的**
`mutation.bench_scale.temp_write`（本卡没有改它，也不是 C65 的测量项）。单独重跑：

    283:     4 GB: open 0.2 ms, sweep 259491.0 ms, extract 262591.1 ms, temp entries 0 -> 1
    283:       temp: staging.7z|4294967418
    283:     FAIL Tests/win/bench_scale.cpp:539  expected (tempAfter.size()) == (0)
    283:     FAIL: the pipeline wrote into %TEMP% (1 entries)
    1/12 Test #283: mutation.bench_scale.temp_write ...   Passed  1228.42 sec

它按预期变红并通过（WILL_FAIL 型负向控制）；20.5 分钟里 sweep 259.5 s、extract 262.6 s，
是磁盘时间，不是挂死（D29 的 TIMEOUT 是 1800 s）。所以“16 分钟跑完全量 bench”在这台
共享机器上不成立，但 12 项都被跑到了：11 项在全量 lane 内，1 项单独重跑。

这一跑里的 C65 是第 5 轮（表中 R5）：首帧 **164.5/183.2 ms**、10k **3.50/5.84 ms**、
100k **4.24/6.18 ms**、串行/并行 **73.9/54.9 ms**、开归档 **76.5/49.9 MB**、
最小化 **71.6/45.2 MB**，与 R2/R3 同带；C49 默认 lane 也再次通过（72.7 s），说明 C65 的
测试改动没有破坏 C49 的口径。

**契约落点**：D1（每条 ctest 都钉 PASS_REGULAR_EXPRESSION，静默 SKIP 不算过）、
D3/D26/D27（mutation.c65.no_first_paint 的 CAUGHT 由运行时谓词决定）、D7（整块替换，无索引手术）、
D9（在 E:\wg\c65 自己的 clone 工作树）、D29（子进程/轮询全部有上限，TIMEOUT 写成真实预算：
c65 900 s、perf_smoke 15 s）。

## 启动到首帧分解（C76，2026-11-03，feat/c76-startup）

C65 给出的数字是 **164.5 ms 进程内首帧 / 208.4 ms p95 / 193.5 ms 黑盒进程→主窗口**，
但那是**一个数**，不是一张表：它说不清 164.5 ms 花在哪一段，所以"要达标该动哪里"只能靠读代码猜。
本节先把这条路径拆成 29 个 QPC 段（每段给出起点、时长、与上一段之间的**空隙**），
再按占用从大到小改，最后给出前后对比、达标判定和**没达标的部分是什么**。

### 0. 怎么测的（哪条命令、哪个进程、什么时刻）

| 项 | 做法 |
|---|---|
| 埋点 | `Src/PrismWin/StartupTrace.hpp`：QPC 单时钟，`Begin()` 在 `wWinMain` 第一行，每段一个 `PRISM_C76_SPAN`/`Add`；关闭时成本是一次 bool 分支 |
| 开关 | 环境变量 `PRISM_C76_TRACE=1`（**只在测量时开**，出厂进程不打印）；`PRISM_C76_TRACE_EXIT=1` 让进程在报告完首帧后自己 `WM_CLOSE`，这样 bench 计的是**真进程**而不是猜的时刻 |
| 被测量的进程 | **出厂的 `PrismArchive.exe`**（不是复刻的探针）：`Start-Process PrismArchive.exe <fixture.7z>`，fixture 是 40 条目 7z |
| 分段口径 | `start_ms` = 相对 `Begin()`；`ms` = 本段时长；`gap_ms` = 本段起点 − 上一段终点（**没埋点的胶水**，摊在桌面上）；名字以 `.` 开头的是**子段**，不计入合计 |
| 机器 | Windows 11 26200，i5-8250U（4 核 8 线程），VS 2022 17.14（MSVC 14.44），Release；**共享机器**，所以同一份二进制多跑几轮并列出轮次 |
| 复现 | `. E:\wg\env.ps1 -Card c76` → `cmake --build E:\build\wt-c76 --target PrismArchive c76_bench_startup` → `ctest --test-dir E:\build\wt-c76 -R "c76|mutation.c76" -V` |

### 1. 分解表（同一台机器、同一个 fixture、同一条命令）

**改之前**（只加了埋点，未做任何优化；`run2`）：

| 段 | ms | 测的是哪个函数前后 |
|---|---|---|
| `settings_language_session` | **133.1**（其中 `.load_language` 40.2 / `.load_settings` 0.4） | `LoadSettings`→`LoadLanguage`→`LoadSession` |
| `hlsl_shaders` | **80.9** | `GlassEffectRuntime::Initialize`：**两次 `D3DCompile`**（Liquid + Kawase） |
| `d2d_hwnd_target` | **53.4** | `GalleryWindow::EnsureDeviceResources`：D2D HWND 目标（内含 D3D11 设备创建/首次加载 d3d11+dxgi） |
| `first_frame_render` | **48.2**（draw 21.0 + `EndDraw` ≈27） | `GalleryWindow::Render`：`BeginDraw`→`renderer_.Draw`→`EndDraw`→Present |
| `session_arm` | **25.0** | `SessionStore::RecordOpen`+`SaveSession`（一次原子写盘） |
| `wiring_tail` | **17.8** | 处理器接线 + companion 预览 + **缩略图扫描线程启动** |
| `settings_menu_session_view` | **17.3** | `InstallMenu`+`SetSettingsLauncher`+`ApplySessionView` |
| `tabs` | **15.4** | `OpenTab`+`SetTabHandler` |
| `view_state` | **12.5** | `RestoreFrame`+`AttachSidebarStore`+`SetListMode` |
| `menu_bar` | **9.4** | `GalleryWindow::RebuildMenu` |
| `d3d11_device` | **8.3** | `D3D11CreateDevice`（玻璃宿主自己的设备） |
| 其余 | `menu_labels` 4.6 / `create_window_ex` 2.9 / `com_argv` 2.7 / `engine_load` 2.3 / `set_items` 2.0 / `dcomp_target` 1.7 / `dwrite_first_format` 1.4 / `d2d_device_context` 1.0 / `ops_plan` 0.6 / `archive_open_catalog` 0.5 / `class_register` 0.03 | — |
| **合计** | **387.2 ms**（`process_start_to_winmain` 另计：34.8 ms，粗粒度 15.6 ms 一格） | |

**改之后**（三轮 374.6 / **325.0** / 307.6，取中位那轮）：

| 段 | ms | 变化 |
|---|---|---|
| `d2d_hwnd_target` | **75.3** | 未改（见 §3） |
| `first_frame_render` | **41.1**（draw 15.7） | `D2D1_PRESENT_OPTIONS_IMMEDIATELY`（不再等垂直回扫） |
| `settings_language_session` | **23.9**（`.load_language` 22.8） | 未改，此段随文件缓存波动 21–114 ms |
| `menu_bar` | **21.8** | 未改 |
| `wiring_tail` | **19.1** | 缩略图线程移到首帧之后（剩下的是 companion 预览 + 接线） |
| `settings_menu_session_view` | **17.4** | 未改 |
| `view_state` | **14.8** | 未改 |
| `tabs` | **14.3** | 未改 |
| `d3d11_device` | **9.3** | 未改（与 `d2d_hwnd_target` 里的设备无法共享，见 §3） |
| 其余 | `create_window_ex` 8.0 / `menu_labels` 3.7 / `engine_load` 3.0 / `dwrite_first_format` 2.8 / `set_items` 2.2 / `com_argv` 1.9 / `dcomp_target` 1.8 / `hlsl_shaders` **1.4** / `d2d_device_context` 1.1 / `ops_plan` 1.0 / `archive_open_catalog` 0.9 / `class_register` 0.04 | `hlsl_shaders` **80.9 → 1.4** |
| `session_arm` | **0**（离开首帧路径） | 见 §2 |
| **合计** | **325.0 ms**（空隙 19.6 ms） | **−62.2 ms（−16.1 %）**；同一段埋点、同一条命令 |

`hlsl_shaders` 与 `session_arm` 是**同一个函数、同一个时钟、改前改后各测一次**，所以那 105 ms 是确定收益；
其余段的差异是机器负载（`load_language` 在一轮里读 21 ms、在另一轮读 114 ms，代码没动）。

### 2. 改了什么（每条都带数字）

1. **着色器改成预编译字节码（.cso），80.9 → 1.4 ms。**
   `Src/PrismWin/CMakeLists.txt` 在**构建时**用 Windows SDK 的 `fxc` 把两个 .hlsl 编译成
   `<build>/Assets/Shaders/*.cso`（同一个入口 / 同一个 `ps_5_0` / 同一个 `/O3`），
   `GlassEffectRuntime` 先读 .cso、读不到才回退 `D3DCompile`。
   回退是硬要求：没有 SDK 的检出必须还能渲染（只慢，不坏）；而且 .cso 头 4 字节不是 `DXBC` 就拒绝。
   计数断言：优化版 `hlsl_precompiled=2`、`hlsl_runtime_compiles=0`（改前 `=2`）。
2. **首帧不合成玻璃岛**：`EnableGlassLayer` 不再立刻 `PresentGlassFrame`，改为置 `glassPresentPending_`，
   由**第二帧**的 `AfterWindowFrame` 提交；`RestoreGlassLayer`（`ShowWindow` 的 `WM_SIZE` 会先到）在挂起期间只重建瞬态表面、不提交。
   岛是**清成透明**再合成到窗口自己的不透明内容上的，所以屏上像素不变——这一点由像素断言守（§4），不是嘴上说。
3. **会话崩溃卫兵的写盘移到首帧之后**（25.0 ms）：`RecordOpen` 仍在原处决定（视图模式要一致），
   只有 `SaveSession` 这一次原子写移到窗口自己的首帧边界。代价写在明处：首帧那一小段里崩溃不再被记录。
4. **缩略图扫描线程移到首帧之后**：它和首帧抢同一个文件缓存与 CPU，而且**它的像素本来就不可能在首帧上屏**；
   移走后首帧更确定（首帧再也不会出现"刚好赶到的缩略图"），`C76_PIXELS` 的 digest 因此可复现。
5. **`D2D1_PRESENT_OPTIONS_IMMEDIATELY`**：第一次 `EndDraw` 不再等垂直回扫（60 Hz 下最多 16.7 ms）；
   像素不变，只是 `Present` 返回前不再睡一觉。
6. （**没做**）字体延迟创建：首帧本来就要画文字，`CreateDeviceResources` 里那 7 个 `CreateTextFormat` 一帧也用得上，
   延迟到"第一次画文字"等于原地踏步——数据不支持，所以不动（`dwrite_first_format` 只有 2.8 ms，也证明了这一点）。

### 3. 达标判定：**未达标**，差 5.4 倍，剩下的就是这些

`C76_BENCH {"target_ms":60.0,"cold_first_frame_ms":193.680,"warm_first_frame_ms":200.216,"inproc_first_frame_ms":162.556,...,"met":false}`

- **进程内首帧 325.0 ms**（C65 的 164.5 ms 是**另一种口径**：它只计 Create+Glass+首帧绘制，不含 settings/language/session/接线；
  同一口径下本卡实测是 `162.6 ms`——和 C65 的 164.5 ms 基本一样，**所以这张卡没有让那个口径变快**，它让**整条路径**变快了）。
- **端到端**：`process_start_to_winmain` 16.5 ms + 325.0 ms ≈ **341 ms**（黑盒口径另计）。
- **60 ms 目标没达到**。剩余部分（改后表里最大的五段，合计 **187 ms，占 58 %**）：
  1. `d2d_hwnd_target` **75.3 ms**：D2D 建 HWND 目标连带 D3D11 设备与 d3d11/dxgi 首次加载。**这是设备创建，不是我们的代码**；
     要消掉只能让 D2D 与玻璃宿主共享同一个 D3D11 设备（改用 `ID2D1Factory1::CreateDevice`+`CreateBitmap` 的合成交换链路径），那是重写渲染后端，不在本卡。
  2. `settings_language_session` **23.9 ms**（`.load_language`）：读 `Assets\Lang\*.json`，冷缓存一轮读到 **114 ms**。它的产出（菜单/侧栏/动作带的字）首帧就要上屏，不能推到首帧后；
     要快只能把目录表预编译成二进制或常驻预热。
  3. `first_frame_render` **41.1 ms**：D2D 画 25 行 + 侧栏 + 面包屑 + 动作带，`EndDraw` 光栅化到 HWND 目标。
  4. `menu_bar` **21.8 ms** + `settings_menu_session_view` 17.4 ms：菜单创建与语言/会话视图装配。
  5. `wiring_tail` **19.1 ms**：companion 预览窗与处理器接线（预览的窗口创建也可以再往后挪，但那是 C30 的地盘）。
  这五段都**不是"某处回归"**，而是"首帧必须建窗口、建设备、画一整帧"。ADR-001 的 60 ms 在当前架构下只有两条路：
  **常驻预热进程**（蓝图有、代码里没有）或**把整个渲染后端改成共享设备 + 交换链**。两者都是另一张卡的体量。

### 4. 断言（哪些是门，哪些只是数字）

`Tests/win/bench_c76_startup.cpp` + `Tests/suites/c76-startup.cmake`，4 个 ctest：

| 测试 | 断言 |
|---|---|
| `c76.bench_startup` | **先证明帧是真的**（`rendered == 1`、`enddraw_hr == 0x0`、`painted >= 5%`、`digest != 0`，C113 新增），再比 digest：**`e735eb87a09d4f84` @1264×741**（C113 重录，见下方“作废/待重测”）；`hlsl_runtime_compiles == 0`；`hlsl_precompiled >= 2`；**首帧玻璃呈现次数 == 0**；第二帧 ≥ 1；首帧 < 1500 ms（防挂死，不是 60 ms 门）。~~第一/二帧差异像素在玻璃岛之外为 0~~ **待重测**（C113 之前它比较的两帧是同一张被 D2D 拒绝的空帧，这条恒真） |
| `c76.bench_baseline` | 改前行为（运行时编译 + 首帧上玻璃）的参考数字与参考 digest。旧值 `79a56a1305dd98de` **作废**（C113 之前的镜头里混入了设备域失败的帧），C113 之后这台机器读出 `e735eb87a09d4f84`，**待重测**并重录 |
| `mutation.c76.runtime_shaders` | 注掉 .cso 路径 → `runtime_compiles=2` → 同一条 `== 0` 谓词变红，打印 `C76 MUTATION CAUGHT: runtime_shaders`（D27：注入先被运行期观测到，才允许宣告捕获） |
| `mutation.c76.glass_on_first_frame` | 恢复"首帧上玻璃" → `presents_frame1=2` → `presents_frame1 == 0` 变红，打印 `C76 MUTATION CAUGHT: glass_on_first_frame` |

**C113 作废/待重测清单（2026-11-07，feat/c113-bench-honesty）**。C113 查实：`DrawFrame` 把 `target->EndDraw()` 的 HRESULT 丢掉了，
于是窗口真实绘制过之后的离屏渲染被 D2D 以 **0x88990012（D2DERR_WRONG_RESOURCE_DOMAIN）** 整帧拒绝，而 bench 照旧把这张
**只有画布底色、`painted == 0`** 的帧当成功帧求 digest。下面这些数字因此不能再当证据，**待重测**（本卡不改数字，只作标记）：

1. 本节所有 C76 **首帧像素 digest**（`79a56a1305dd98de`）——旧值来自改卡前的构建，无法再由真实帧产生；C113 重录为 `e735eb87a09d4f84`。
2. 本节 `c76.bench_baseline` 的参考 digest（同上）。
3. 本节“第一/二帧差异像素在玻璃岛矩形之外为 0”——C113 之前它比较的两帧都是同一张被拒绝的空帧，所以**恒真**；真实帧上的值**待重测**。
4. C112 在 `docs/PERF.md` 里把 digest 改录成 `0af0b75bc5b12383` 的那一行：该值正是被拒绝的空帧（C113 的 H1 在本机复现出同一串数字，`painted == 0/936624`），**作废**。
5. `C76_BENCH.first_frame_digest` / `C76_PIXELS.frame1|frame2` 的历史引用值——**待重测**。

毫秒数（cold/warm/inproc 首帧）走的是另一条路径（真实 WM_PAINT），本卡没有发现它们被污染，保留。

**为什么 60 ms 不写成断言**：契约 R2（AGENTS.md）——共享机器上的秒表不可复现，同一份二进制在这里读出 274/307/325/375 ms。
门是**计数与像素**（`hlsl_runtime_compiles==0`、`presents_frame1==0`、digest 相等、差异只在玻璃区），
毫秒进 `C76_BENCH` 的 `met` 字段，**照实报告未达标**。

**digest 的边界**：布局由客户区宽度驱动，所以像素相等只在 1264×741 下有意义；换尺寸时 bench 打印 `C76 NOTE` 并跳过这条相等断言，
而不是把"另一个窗口尺寸"谎报成像素回归。

**负向控制的另一面**（R1：注释不是证据）：把 `.cso` 路径注掉后 `hlsl_runtime_compiles` 真的变 2（打印在同一行 `C76_PIXELS` 里），
把玻璃推迟开关注掉后 `presents_frame1` 真的变 2——两个数都是**运行时观测**，不是宏定义存在与否。
