# DEVLOG

STAR-format log of problems that cost real time. Entries are written the day the
problem was hit, including the wrong guesses.

---

## 2026-08-26 The pager was silently losing writes

**Situation.** Every run reported plausible-looking counters, and nothing
crashed. But a probe that wrote `0xAA` into page 0, forced the page out, and then
read the backing file directly found `0x00` on disk — the pattern byte. The write
had been discarded with no error anywhere.

**Task.** Confirm the pager preserves data before trusting any measurement it
produces.

**Action.** First guess was the write-back path in `evict_frame`, or a wrong file
offset in `write_page`. Both were fine — the store round-trip test passed. The
actual cause was in `handle_page_fault`: the read branch ran
`entry.dirty = false` on *every* read fault. A page that had been written, then
demoted to `PAGE_NOACCESS` by the clock hand, then merely **read**, had its dirty
bit cleared. The page still held the modification in memory, but the pager now
believed it matched the store, so eviction skipped the write-back.

**Result.** The dirty bit is now cleared in exactly one place, `commit_page`,
where a page is freshly loaded and genuinely does match the store. A read fault
on a dirty page restores `PAGE_READWRITE` rather than `PAGE_READONLY`, since
there is nothing left to learn about a page already known to be dirty. Test F4
now writes a signature into all 100 pages, forces them all out, and re-reads the
file directly; it fails if a single page is lost. Beyond correctness, this bug
was quietly deflating the write-back counter — the exact quantity the whole
project claims to reduce.

---

## 2026-08-26 The Zipf generator was quadratic

**Situation.** `config/debug.ini` — only 5,000 accesses over 256 pages — took
13.7 seconds. The full 200,000-access experiment never finished.

**Task.** Find out whether the cost was the unbuffered I/O or something else.

**Action.** Assumed it was the SSD, since `FILE_FLAG_NO_BUFFERING` makes each
4 KB transfer a real device operation. Timed the workload generator on its own to
rule it out, and it did not get ruled out: 1,000 draws took 0.068 s, 2,000 took
0.19 s, 4,000 took 0.83 s, 8,000 took 3.40 s. Doubling the count quadrupled the
time. `workload_next` was constructing a fresh `std::mt19937`, seeding it, and
replaying it from the beginning on every single call, to reach draw number N.

**Result.** One engine now lives in the `Workload` struct and is advanced once
per draw, so a draw costs one RNG step plus a binary search. Extrapolating the
old curve, 200,000 accesses would have spent roughly six hours inside the
generator alone before touching a page. This was the difference between a sweep
being impossible and taking twenty minutes.

---

## 2026-08-26 Every measurement column in the CSV was zero

**Situation.** The CSV had columns for faults, page-ins, evictions and I/O
latency, and a committed `sample_runs.csv` full of confident-looking numbers
(`io_read_us = 78.4`, `r_hat = 5.26`). A fresh run wrote zeros into all of them.

**Task.** Work out which numbers the program could actually produce.

**Action.** The `stats_record_*` and `stats_update_io_*` functions existed and
compiled, but nothing ever called them. The arena counted faults internally in
`arena.fault_count`, and that value was never copied into `Stats`. No code
anywhere called `QueryPerformanceCounter`, so no latency had ever been measured.
The committed sample row could not have come from this program.

**Result.** The store now times every `ReadFile` and `WriteFile` with QPC, the
experiment copies the arena's counters into `Stats` at the end of the run, and
`pages_read`/`pages_written` were split into `read_accesses`/`write_accesses`
(what the workload asked for) and `pagein_reads`/`writeback_writes` (4 KB
transfers actually issued to the device). Those two meanings differ by more than
an order of magnitude and the old single column conflated them. The fabricated
`sample_runs.csv` was deleted and regenerated from a real sweep.

---

## 2026-08-26 Two identical runs disagreed about how long the disk took

**Situation.** Test F7b runs CLOCK and CA-CLOCK on a sequential workload. Both
issued *exactly* 8,138 write-backs and 20,000 page-ins — the operation counts are
deterministic. The reported total I/O time was 14,383 ms for one and 28,399 ms
for the other.

**Task.** Decide whether CA-CLOCK had doubled the cost, or whether the
measurement was wrong.

**Action.** Since the operation counts were bit-identical, no policy difference
could explain a 2x cost difference; the only variable left was per-operation
latency. The cost figure was being computed as `operations x this run's own
measured latency`, and device latency drifts a long way between runs on a laptop
with background activity. One sweep run later reported a modelled 104 s of I/O
against a 27 s wall clock, which is impossible and confirmed it.

**Result.** Measurement and cost were separated. Operation counts come from the
runs and are reproducible. Cost comes from `ca-clock.exe --calibrate`, which
takes 200 random 4 KB reads and writes and reports **medians** — a single 40 ms
outlier from a background process moves a mean far more than it moves the typical
operation. The analysis applies that one calibration to every run being compared.
Figures are labelled "modelled cost, measured operation counts".

---

## 2026-08-26 Writes turned out to be cheaper than reads

**Situation.** The project's premise is that a write costs more than a read.
The first calibration on this laptop returned `r_hat = 0.47` — a 4 KB write cost
*half* what a read cost. On that device, CA-CLOCK's trade would be a loss.

**Task.** Find out whether the calibration was broken or the premise was.

**Action.** Checked the drive first: the repository is on `E:`, which is disk 1,
a `ST1000LM035` 5400 rpm HDD — not the 128 GB SSD on `C:`. That explained slow
reads but not fast writes. The answer was `durable_writes`. With
`FILE_FLAG_WRITE_THROUGH` but no `FlushFileBuffers`, a write lands in the drive's
cache and returns; a read has to fetch from the platter. Re-running calibration
with `durable_writes = true` gave a write median of 21,692 us against a read
median of 930 us: `r_hat = 23.3`.

**Result.** Not a bug — the premise is conditional, and the condition is
durability. The same machine gives `r_hat = 0.47` or `r_hat = 23.3` depending on
one config flag, a factor of 50. This is now reported as a finding rather than
assumed away, and both modes are in the README.

---

## 2026-08-26 CA-CLOCK stops being CLOCK at high alpha

**Situation.** Sweeping α from 0 to 1 at write ratio 0.40 looked ideal: both
write-backs and page-ins fell monotonically, and α = 1.0 cut write-backs by 98%.
It seemed α = 1 simply dominated, which would make α an uninteresting knob.

**Task.** Check whether α = 1 is genuinely free before claiming it.

**Action.** Added a counter for the case where the bounded scan sweeps
`2 x capacity` frames, finds every candidate spared, and has to evict whatever
sits under the hand. At write ratio 0.40 it was zero, which is why α = 1 looked
free. Re-ran at write ratio 0.80: **809,990** sparing decisions, and **59%** of
evictions fell through to that give-up path. Wall-clock time rose from 19 s to
24 s despite *fewer* I/O operations, because the hand was spinning.

**Result.** At high α with a mostly-dirty frame ring, the policy degenerates into
near-random replacement with large CPU overhead. This is the known weakness of
unbounded clean-first replacement (CFLRU's clean-first region), and it is now
measured rather than hidden: `scan_exhausted` is a CSV column and Figure 3 plots
it. Any α result has to be read next to that number.

---

## 2026-08-26 Replacing the probability with a counter

**Situation.** The previous entry left α = 1.0 degenerating into near-random
replacement at high write ratios. The α knob had no upper limit on how long a
dirty page could dodge eviction, so under write pressure every frame ended up
spared and the hand simply spun.

**Task.** Remove the failure mode without abandoning the idea of sparing dirty
pages, and without adding much code.

**Action.** First thought was to lower α, but that only makes the collapse less
likely rather than impossible, and it reintroduces the magic-constant problem. The
fix was to change what the knob *is*: instead of skipping a dirty page with
probability α, skip it at most **k** times, tracked in a per-page counter that
resets when the page is referenced again. `k = 0` is plain CLOCK; `k = 1` is close
to LRU-WSR's cold-detection bit.

Getting the scan bound right took a second pass. The existing limit of
`2 x capacity` sweeps is enough for CLOCK — one lap to clear reference bits, one
to pick a victim — but not for bounded-k, where a frame can legitimately be passed
k times before it must go. The bound is now `(k + 2) * capacity`, which is exactly
what makes the guarantee hold: one lap clearing R, then k laps spending skip
budgets, and on encounter k+1 a frame *must* be evicted. At `k = 0` it reduces to
the old bound, so the CLOCK and probabilistic results are unchanged.

**Result.** Bounded-k does not just avoid the collapse, it beats the collapsed
policy outright. At write ratio 0.80: α = 1.0 gives −7.7% modelled cost with 59.2%
of evictions exhausted, while `k = 4` gives **−10.2%** with **0.0%** exhausted and
36x fewer sparing decisions. Test F9 asserts `scan_exhausted == 0` for bounded-k
rather than "small", because the bound makes it a guarantee rather than a
tendency. About 20 lines of policy change, and the thing it fixes was measured
first.

---

## 2026-08-26 Sweep runs failed with sharing violations

**Situation.** The first run of a sweep succeeded; the next four failed with
`CreateFileA (reset) failed with GetLastError()=32`.

**Task.** Keep the sweep running unattended.

**Action.** Error 32 is `ERROR_SHARING_VIOLATION`. The previous process had
exited and closed its handle, so nothing of ours held the file — antivirus and
the search indexer open newly written files to scan them, and the next process
arrives before they let go.

**Result.** File opens retry for up to one second on `ERROR_SHARING_VIOLATION`
and `ERROR_ACCESS_DENIED`, and the reset handle now allows `FILE_SHARE_READ`
instead of demanding exclusive access. Sweeps run unattended.
