# Milestone 2 design: LFU, ARC, W-TinyLFU, Belady

All four plug into the existing `EvictionPolicy` hooks and the shared `Cache` (decisions D1–D3).
This doc covers the interface changes they need, then each policy's data structures, complexity,
and how it is tested.

## 0. Interface changes

### 0.1 Policies are constructed with the cache capacity
ARC needs `c` (its target sizes and ghost bounds are fractions of it), and W-TinyLFU needs it to
split the window and main segments. So `make_policy(name, capacity_bytes)`. LRU, LFU and Belady
ignore it.

### 0.2 The new object may be the victim, and each policy decides whether it can be
Under insert-then-evict (D2), the newcomer is already tracked when `evict()` runs. That's exactly what
admission-style policies need (W-TinyLFU's filter, Belady skipping an object it will never reuse). For the
classic policies it would be wrong: LFU would always evict the newcomer, which has frequency 1, so the cache
would freeze. ARC's REPLACE is defined *before* the new page is added.

**Rule:** LFU and ARC never choose the object inserted by the current access, unless it is the only
object left (which can't happen, since an object that fits never needs every other object evicted
*and* itself). This makes them behave exactly like their evict-then-insert definitions. LRU gets this
for free (the newcomer is the most recent). W-TinyLFU and Belady may pick the newcomer on purpose.

### 0.3 Future knowledge for Belady: `Request::next_vtime`
`Request` gets `next_vtime`: the index (0-based, in the replayed stream) of the next request
for the same key, or `kNever`. It's filled by a pre-pass, `annotate_next_access()`, which loads
the trace into memory and walks it backwards with a last-seen hash map (O(N) time, O(N) memory).

- `EvictionPolicy::needs_next_access()` (default false). `simulate()` runs the pre-pass only for
  policies that ask for it, after applying `max_requests`, so "next access" means within this run.
- I don't reuse the oracleGeneral file's own `next_access_vtime`: its indices count size-0 records we
  skip (D4), and CSV traces don't have it. One pre-pass works for every format.
- The same field is the label for the M3 model ("time to next access").
- Memory: about 24 bytes per request in memory (113k requests is ~3 MB). Traces with billions of requests will need
  sampling (M3); noted as a limitation.

## 1. LFU (in-cache LFU, ties broken by least recently used)

**Semantics.** A key's frequency counts accesses since it was last inserted; it resets on
eviction ("in-cache" LFU, the same as libCacheSim's LFU). Evict the lowest frequency; among equal
frequencies, evict the key that reached that frequency earliest. Since any later access would
have moved it to a higher frequency, that's the least recently used one.

**Data structure (O(1) LFU, Shah et al. 2010).**
```
buckets_: std::list<Bucket>            Bucket { uint64 freq; std::list<Key> keys; }  // ascending freq
index_:   unordered_map<Key, Entry>    Entry  { bucket iterator; key iterator }
```
- hit: move the key to the bucket `freq+1` (the next bucket, or a new one inserted right after it);
  delete the old bucket if it's now empty. O(1).
- insert: push to the back of the `freq==1` bucket at the front (creating it if needed). O(1).
- evict: the front key of the first bucket, skipping the newcomer (0.2). The newcomer is at the
  back of the freq-1 bucket, so it's only at the front when it's the bucket's only key. Then
  take the front of the next bucket instead. O(1).

A min-heap keyed by (freq, last access) would be O(log n). An `unordered_map<freq, list>` plus
`min_freq` (the LeetCode version) can't find the next-smallest frequency in O(1), and with
insert-then-evict, protecting the newcomer needs exactly that.

**Tests.** Hand-built orders (frequency beats recency; ties go to LRU; frequency resets after
eviction; the newcomer survives when it's the only freq-1 key). Plus a differential test against a
naive O(n) reference written in the textbook evict-then-insert form. This also checks that rule
0.2 makes the two orders equivalent.

## 2. ARC (Megiddo & Modha, FAST '03)

**State.** Two resident LRU lists T1 (seen once recently) and T2 (seen at least twice), and two ghost LRU
lists B1, B2 (keys only, recently evicted from T1/T2). A target size `p` for T1 adapts between 0 and c.
Generalized to bytes the way libCacheSim does it: all list sizes, `p` and `c` are in bytes, and `p` is a double.

**Mapping the paper onto the hooks.**
- `on_hit(x)` (x in T1 or T2): move x to the MRU end of T2 (Case I).
- `on_insert(x)`: the paper's Cases II–IV, *minus* REPLACE.
  - x in B1 (Case II): `p = min(c, p + size(x) * max(1, |B2|/|B1|))`; move x into T2.
  - x in B2 (Case III): `p = max(0, p - size(x) * max(1, |B1|/|B2|))`; move x into T2; remember
    "x came from B2" for REPLACE's tie rule.
  - otherwise (Case IV): put x in T1.
  For unit sizes, `size(x) * max(1, ratio)` is exactly the paper's δ.
- `evict()`: REPLACE(x, p), with x (the current newcomer) excluded from the sizes and from being
  chosen. If `|T1 \ x| > 0` and (`|T1 \ x| > p`, or x came from B2 and `|T1 \ x| == p`), move the LRU of T1
  to the MRU end of B1; otherwise move the LRU of T2 to B2 (falling back to T1 if T2 has nothing but x).
- **Ghost trimming** runs after an insert or evict, but only once the resident lists fit in `c`: drop the LRU
  of B1 while `|T1|+|B1| > c`, then the LRU of B2 while `|T1|+|T2|+|B1|+|B2| > 2c`. For unit sizes this
  gives the same state as the paper's Case IV bookkeeping, just done afterwards instead of before.
  *Found by the differential test:* trimming while the newcomer is still unpaid (resident > c) wrongly
  dropped a B2 ghost in Case IV-A with |T1| = c, where the paper keeps it.

REPLACE only runs when the cache is actually over capacity, because `Cache` only calls `evict()` then.
That matches the paper whenever the cache is full, and handles explicit removes gracefully.

**Data structures.** Four `std::list<Key>` plus one `unordered_map<Key, Entry{which list, iterator, size}>`.
Every operation is O(1) average, except ghost trimming, which costs O(1) amortized (each ghost is trimmed once).
Memory: resident and ghost entries are each bounded by c, so at most about 2c entries in total.

**Tests.** Hand-built unit-size scenarios from the paper (a ghost hit in B1 grows p, a ghost hit in B2 shrinks p,
a key seen twice is protected in T2, scan resistance). Plus a differential test against a reference ARC
transcribed line by line from the paper's pseudocode (Fig. 4, unit sizes, evict-then-insert),
driven through `Cache` with unit sizes: every hit/miss, every eviction, and `p` must match.

## 3. W-TinyLFU (Einziger, Friedman & Manes, 2017; Caffeine's design)

**Structure.**
- **Window**: LRU, 1% of capacity (bytes). Every new object enters here.
- **Main**: segmented LRU, *probation* (20% of main) + *protected* (80% of main).
- **TinyLFU filter**: a count-min sketch of access frequencies (`sketch/count_min_sketch.h`).

**Flow.**
- Every access (hit or miss) increments the key in the sketch.
- Window hit: move to window MRU. Probation hit: promote to protected MRU; if protected is now
  over its budget, demote protected's LRU to probation MRU (no eviction). Protected hit: move to MRU.
- `evict()`: while the window is over budget, its LRU becomes the *candidate* and moves to probation.
  When the cache is over capacity, pick a *victim* (probation LRU, or protected LRU if probation is empty)
  and duel: `estimate(candidate) > estimate(victim)` admits the candidate and evicts the victim;
  otherwise the candidate is evicted. Ties reject the candidate, as in Caffeine. If there's no pending
  candidate, evict the main victim; if main is empty, evict the window's LRU.
- The newcomer *can* be the candidate (e.g. it's larger than the whole window), and then it can lose.
  That's admission control working as intended.

**Count-min sketch.** Depth 4, width a power of two, saturating 4-bit counters (stored as `uint8_t`,
capped at 15, as in Caffeine). Row indices come from double hashing a splitmix64 mix of the key, so
results are deterministic. **Aging:** after `10 × width` increments, halve every counter (TinyLFU's
"reset"), so old popularity fades.
**Sizing:** width = next power of two ≥ the high-water mark of resident objects (min 16). When the
resident count grows past the width, the sketch is rebuilt at double the width (counts are lost;
this happens O(log n) times, all during warm-up). Caffeine does the same when its maximum size
changes. The object count of a byte-sized cache isn't known up front, which is why this is needed.

**Deferred** (decision log): Caffeine's hill-climbing adaptive window size, the doorkeeper Bloom
filter, and random admission for hash-flooding protection. The fixed 1% window is the published
W-TinyLFU baseline.

**Complexity.** O(1) average per access (four sketch counters + list moves). Memory: O(n) for the
lists plus `4 × width` bytes for the sketch.

**Tests.** Sketch: never underestimates before aging, saturates at 15, halves on aging, deterministic.
Policy: hand-built admission duels (a frequent candidate beats a rare victim and vice versa, ties
reject), probation→protected promotion and demotion, the newcomer being rejected. Property tests:
on a fixed-seed scan-plus-hot-set trace and a Zipf trace, W-TinyLFU beats LRU; cache invariants
hold on random variable-size traces.

## 4. Belady (MIN / OPT)

**Semantics.** Evict the cached object whose next access is furthest in the future (or never).
Because of insert-then-evict, the newcomer competes too, so Belady can *skip caching* an object it
will never need again. For unit sizes, that's the true optimum (Belady's MIN with bypass).
For variable sizes, furthest-next-access is a well-known heuristic, **not** the optimum (byte-optimal
caching is NP-hard). Reports will call it "Belady (furthest next access)".

**Data structures.** `std::set<pair<next_vtime, Key>>` (max = victim) plus `unordered_map<Key, next_vtime>`.
Every operation is O(log n).

**Tests.** Hand-built cases (furthest evicted; never-again evicted first; the newcomer skipped when its
own next access is furthest). And an **optimality test**: on many small random unit-size traces,
Belady's hit count must equal the true optimum found by exhaustive search (memoised over (time, cache
contents)). Also: Belady ≥ every other policy on the same trace.

## 5. Milestone 2 deliverables
- Unit tests for each policy, written before its implementation.
- `cachesim -p lru,lfu,arc,wtinylfu,belady` and a results table at 3 cache sizes (`docs/m2-results.md`).
- An informative comparison against libCacheSim's LFU/ARC/W-TinyLFU/Belady. Where we differ by
  design (Belady skipping objects, a sketch instead of a counting Bloom filter), the difference is
  explained, not tuned away.
