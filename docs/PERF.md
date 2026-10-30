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
