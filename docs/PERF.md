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
**这台机器是共享的**（七个兄弟工作树在编译），所以同一份二进制跑了两轮：A 轮（安静）与 B 轮（有兄弟编译）。
两轮都列出来，因为差异不是代码，是机器——这正是上面 ch.7.2 一节用过的记账方式。

### 1. 冷启动 → 首帧（装配后的真实窗口）

| 采样 | 中位数 | p95 |
|---|---|---|
| A 轮：空作用域 → 首帧画完（CreateWindowExW + 渲染器资源 + 菜单 + 玻璃宿主 + 首次 WM_PAINT） | **311.0 ms** | **441.1 ms** |
| B 轮：同上（带拆分） | **363.8 ms** | **581.2 ms** |
| └ 其中 `Create`（窗口 + 设备无关资源 + 菜单） | 91.8 ms | — |
| └ 其中 `EnableGlassLayer`（DComp 宿主 + 2 次 HLSL 编译 + 首帧玻璃呈现） | 32.5 ms | — |
| └ 其中**首次 WM_PAINT**（`BeginDraw` + 列表/侧栏/面包屑 + `EndDraw`） | 239.7 ms | 379.2 ms |

**与上一次的差**：旧数字 **22.7 ms** 是"3 个玻璃岛的 BeginDraw+EndDraw+Present+Commit"，
**不含**建窗、不含渲染器的设备资源、不含玻璃宿主的初始化。装配后单是**首次绘制**就是
**204–240 ms**（A 轮总 311.0 − Create/glass ≈ 107 → 绘制 ≈ 204 ms），整条冷启动路径是 **311–364 ms**。
**是否达标**：ADR-001 的 **首帧 ≤ 60 ms → 未达标**（差 4–6 倍）。
原因不是某一处回归，而是旧数字**从来没有包含**这几段：装配后的首帧必须建窗口、建 D2D/DWrite 资源、
编译两个玻璃效果、再画一整帧 1280×800 的列表 + 侧栏 + 面包屑。**目标数字不改**；
要达标只有两条路：预热的常驻进程（蓝图里有、代码里没有），或把设备相关资源的创建挪出首帧路径。
B 轮的 581 ms p95 是兄弟编译的负载敏感性，与 C7 记录的 347.7 ms 同性质。

### 2. 常驻内存：四种状态（ADR-006 分档）

| 状态 | 中位数 | p95 | ADR-006 档 | 判定 |
|---|---|---|---|---|
| 空窗口（无归档、无玻璃） | **54.8 MB** | 55.0 MB | —（无档） | 记录 |
| + 1000 条目 catalog（V1 布局 + C29 侧栏，列表模式） | **61.1 MB** | 61.5 MB | — | 记录 |
| + 玻璃岛活体（`EnableGlassLayer` 并呈现） | **63.9 MB** | 63.9 MB | 玻璃全开 ≤ 96 MB | **达标** |
| **出厂 `PrismArchive.exe`**，归档已打开（= 玻璃全开） | **77.5–78.6 MB** | 78.4–79.9 MB | 玻璃全开 ≤ 96 MB | **达标** |
| **出厂 `PrismArchive.exe`**，最小化后 | **73.6–74.0 MB** | 74.7–75.2 MB | 最小化 ≤ 40 MB | **未达标** |

**与上一次的差**：旧的常驻数字是 **58.4 MB**（黑盒、7 条目归档、玻璃 UI 已落地但 C29/C45 未合入）。
现在出厂进程打开归档是 **77.5–78.6 MB**，**+19`20 MB**，方向与 C29 侧栏（树 + 遥测 + 收藏/最近）
和 C45 的缩略图缓存一致；空窗口的地板（**54.8 MB**）说明这 20 MB 不是一次性开销，
而是布局/侧栏/缩略图真的常驻。

**最小化那一档为什么差 34 MB**：C33 的 +40.33 MB 是**满窗**玻璃岛的账（1600×1000，3 pass）；
C34 把岛改成**导航胶囊自己的矩形**（960×48），所以最小化能还给系统的瞬态字节只剩 **`3.5–4.0 MB**
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

**与上一次的差**：旧数字是 **7.9 ms 中位 / p95 15.6 ms**（C33 自己另记 11.6/20.1）。
A 轮 **4.20 / 6.58 ms** 是**两半都变快**（draw 3.4–4.2 ms，flush 3.5–4.7 ms），
与 V1 把布局收敛成一次计算、C29 侧栏只画命令列表的方向一致。
**是否达标**：120 FPS 的 **8.33 ms** 预算，A 轮**中位数与 p95 都达标**（6.58 < 8.33）；
B 轮**未达标**（p95 40.3 ms）。两轮之间 4 倍的差是机器负载，不是代码——所以结论写成
**"安静机器上达标、满载时未达标"**，而不是把 A 轮当成保证。虚拟化仍然精确：1 千行与 10 万行都只画 **25 行**。

### 5. 800 张缩略图：串行 vs 并行（C45 流水线复跑）

| 轮次 | 串行中位数 | 并行中位数 | 加速比 | worker | 像素 |
|---|---|---|---|---|---|
| A 轮（各 5 轮） | **80.7 ms**（p95 83.5） | **60.3 ms**（p95 63.4） | **1.34×** | 4 | 逐字节相同 |
| B 轮 | 106.0 ms（p95 241.3） | 74.8 ms（p95 101.9） | 1.42× | 4 | 逐字节相同 |

**与上一次的差**：C45 的两臂在**装配后**复跑，形状不变，但加速比只有 **1.34–1.42×**（4 worker）：
这一格的瓶颈不在解码——800 次 128×128 合成解码本身只要 `80 ms，余下的是 `PostMessage`
交接 + UI 线程逐条 apply。**是否达标**：这一格**没有目标数字**（C45 的契约是"每条只解码一次、
第二次 100 % 命中、像素逐字节相同、常驻不超 64 MiB"），四条**全部达标**
（`decoded=800`、`second_pass_hits=800`、两轮 `pixel_digest` 相同、`resident=50 MiB ≤ 64 MiB`）。

### 6. 与 ADR-006 分档的对照（一句话结论）

| ADR-006 条目 | 预算 | 实测（装配后） | 判定 |
|---|---|---|---|
| 玻璃全开（出厂进程，归档打开） | ≤ 96 MB | **77.5–78.6 MB** | **达标**（余量 17–19 MB） |
| 最小化 / 无岛可见 | ≤ 40 MB | **73.6–74.0 MB** | **未达标**：只还回 3.5–4.0 MB，因为岛是 960×48（C34）而 74 MB 的主体不是瞬态 |
| 缩略图缓存（独立预算） | ≤ 64 MiB | **50 MiB**（800 × 128² 满载） | **达标**（D17 的逐字节容量契约由 `core.test_thumb_budget` 守） |
| 首帧（ADR-001，未改） | ≤ 60 ms | **311–364 ms** | **未达标**（见 §1 的拆分） |
| 120 FPS（未改） | ≤ 8.33 ms | **4.20 / p95 6.58 ms**（A 轮）；17.5 / 40.3（B 轮） | **安静达标，满载未达标** |

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
