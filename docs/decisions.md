# Design decisions

A running log of design decisions and the alternatives we rejected. Newest entries at the bottom.

## D1. Policy interface: hooks + a shared `Cache` (M1)

**Decision.** `EvictionPolicy` exposes hooks (`on_hit`, `on_insert`, `evict`, `on_remove`). `sim::Cache`
owns the key→size map and byte accounting and asks the policy for victims.

**Rejected.** The libCacheSim-style `bool access(req)`, where each policy manages its own capacity.

**Why.**
- Capacity semantics (byte accounting, objects larger than the cache) are implemented once, so every policy is
  compared under identical rules.
- Eviction order can be unit-tested directly by calling `evict()`.
- The same policies can be reused by the network server (M5), whose store holds values.

**Cost.** Two hash maps per access (Cache + policy). Acceptable for simulation; revisit if profiling shows it.

## D2. Insert, then evict (M1)

On a miss the Cache calls `on_insert` first and then `evict()` until `used <= capacity`. This lets admission-
style policies (W-TinyLFU, Learned) choose the newcomer itself as the victim. For LRU the newcomer is the MRU
item, so this is equivalent to libCacheSim's evict-then-insert order. Objects larger than the whole cache are
never inserted (counted as a miss).

## D3. Request semantics follow libCacheSim (M1)

- Every request is treated as a GET; a miss inserts the object.
- A key's size is the size of its first request; later size changes on hits are ignored.
- No warmup period; hit rate is over all requests.

These match libCacheSim so the M1 LRU cross-check can be an exact comparison.

## D4. Trace formats and keys (M1)

- Primary format: libCacheSim `oracleGeneral` binary — 24-byte little-endian records
  `{u32 time, u64 obj_id, u32 obj_size, i64 next_access_vtime}`. Verified against libCacheSim's
  `oracleGeneralBin.h` and `data/cloudPhysicsIO.oracleGeneral.bin` at commit `93529a1`. In that file
  `next_access_vtime` is 1-based (the next access's 0-based index + 1) and -1 means "never again".
- **Requests with size 0 are skipped by default**, because libCacheSim's reader does the same
  (`ignore_size_zero_req = true`). Without this, request counts would not match.
- `next_access_vtime` is not stored in `Request` for now; Belady (M2) and ML labels (M3) will compute
  next-access times in their own pass, so they also work on formats that don't include it.
- Fields are read with `memcpy`, never by casting the buffer pointer, which would be an unaligned access (UB).
- String keys (CSV) are hashed with FNV-1a 64-bit, not `std::hash`, so results are the same on every
  platform and standard library.

## D5. Running TSan on WSL2 (tooling)

The TSan runtime shipped with GCC 11 / LLVM 14 aborts on this kernel with
`FATAL: ThreadSanitizer: unexpected memory mapping`, because the kernel uses more ASLR entropy than that
runtime supports. Workaround without root: run TSan tests with ASLR disabled for that process:

    setarch "$(uname -m)" -R ctest --preset tsan

GoogleTest discovery is set to `PRE_TEST` so the build itself never has to execute a TSan binary.
(Alternatives: `sudo sysctl vm.mmap_rnd_bits=28`, or a newer compiler whose TSan supports this.)

## D6. Simulator details (M1)

- `Cache` holds a non-owning reference to its policy, so tests can inspect a fake policy directly.
  The policy must be fresh for every run; `simulate()` rewinds the reader so one reader can be replayed
  for several cache sizes.
- `ignore_size` sets every object's size to 1, so the capacity counts objects (libCacheSim's `--ignore-obj-size`).
  Byte hit rate still uses the real sizes, so it stays meaningful.
- `Cache` throws `std::logic_error` if a policy evicts a key the cache doesn't hold, rather than
  silently corrupting the byte count or looping. This guards against bugs in the learned policy (M3/M4).

## D7. `Request::next_vtime` and the annotation pre-pass (M2; revises D4)

D4 kept next-access information out of `Request`. Belady (M2) and the M3 labels both need it, so
`Request` now has `next_vtime`, filled by `annotate_next_access()`: load the trace into memory and
walk it backwards with a last-seen map. The oracleGeneral file's own field is still ignored: its
indices count the size-0 records we skip, and CSV traces don't have it. Cost: O(N) memory,
about 24 bytes per request in memory. Very large traces will need sampling.

## D8. ARC with variable sizes differs from libCacheSim's ARC (M2)

Both match the paper (and each other) exactly with unit sizes. With byte sizes, ours differs from
libCacheSim by −0.26 to +1.64 pp on cloudPhysicsIO. The paper is defined for unit pages, so byte
ARC has no canonical form:
- **Ours:** insert-then-evict; ghost lists trimmed to the paper's bounds (|T1|+|B1| ≤ c,
  total ≤ 2c, in bytes) once the resident set fits; p moves by `size × max(1, ratio)`.
- **libCacheSim:** evict-then-insert; deletes at most one ghost per miss; p moves by
  `max(1, ratio)` bytes regardless of object size.

**Tested hypothesis (rejected):** p's step size is not the cause. With libCacheSim's unscaled step,
ours gives 21,008 / 30,242 / 49,505 hits vs libCacheSim's 21,300 / 28,011 / 49,100, no closer.
We keep the size-scaled step: with unit sizes it is exactly the paper's δ.

## D9. W-TinyLFU sketch growth: forget counts (kept) vs keep counts (rejected) (M2)

The sketch starts 16 wide and doubles when resident keys exceed its width. Because widths are
powers of two, doubling *could* keep every estimate (copy each counter into column j and
j + old width). We measured both on cloudPhysicsIO (hits):

| config | forget on growth (kept) | keep counts | change |
|---|---|---|---|
| 64 MiB | 22,470 | 22,906 | +0.38 pp |
| 256 MiB | 33,088 | 26,802 | −5.52 pp |
| 1 GiB | 54,257 | 59,552 | +4.65 pp |
| 100 objects | 14,596 | 14,797 | +0.18 pp |
| 1,000 objects | 20,418 | 20,069 | −0.31 pp |
| 10,000 objects | 39,604 | 30,530 | −7.97 pp |

Keeping counts is not better overall: it wins 3 configurations and loses 3, by up to 7.97 pp.
Likely cause: in the narrow early sketch most counters saturate from collisions, and keeping them
makes old keys look maximally popular, so new candidates are rejected until aging catches up.
Picking whichever variant suits this one trace would be overfitting, so the original design stays.
Revisit when sizing the sketch from an object-count estimate, or with Caffeine's adaptive window (M3+).
