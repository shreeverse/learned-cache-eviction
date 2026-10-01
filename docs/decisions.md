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
