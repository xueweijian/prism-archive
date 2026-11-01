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
