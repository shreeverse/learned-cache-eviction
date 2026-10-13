# M5 design: sharded RESP server

Approved 2026-10-13. Decisions are logged in `decisions.md` (D22).

**Goal.** A thread-safe cache server that reuses the M1–M4 policies unchanged, speaks enough of the
Redis protocol (RESP2) for `redis-cli` and `redis-benchmark`, and is measured for ops/sec and p99
latency, TSan clean.

## 0. Constraints from the existing code

- `EvictionPolicy` tracks metadata only; the caller owns key → size and byte accounting (D1). The
  server's `Shard` plays the role `sim::Cache` plays in the simulator, but also stores values.
- `Predictor::predict()` is non-const and `LearnedPolicy` owns an RNG and scratch buffers, so
  policies and predictors are **not** shareable: one policy (with its own predictor) per shard.
  Models are small (32–128 trees), so the duplication is cheap.
- Policy keys are `uint64_t`; Redis keys are byte strings (section 3).

## 1. Threads and shards

**N I/O threads, each running its own `asio::io_context`; S mutex-protected shards, S ≫ N; any
thread may lock any shard.**

- One acceptor assigns connections round-robin; a connection lives on one thread for its whole
  life, so its handlers never run concurrently (no strands needed).
- Shard = `hash(key) & (S - 1)`; S is a power of two (default 64). Capacity per shard =
  total / S.
- With S ≫ N, two threads contend on a mutex with probability about N/S. Each `Shard` is
  `alignas(64)` to avoid false sharing between neighbouring locks.
- There is no fixed thread-to-shard mapping.

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| io_context per thread + locked shards | Simple; one hop per request; memcached-style | A lock per op | **Chosen** |
| Shared-nothing (threads own shards, forward requests, Seastar/Dragonfly-style) | No locks | With random keys (N−1)/N of requests hop threads; needs queues and reply reordering under pipelining | Rejected for v1 |
| One io_context, N threads calling `run()` | Least code | Strands per connection; handlers migrate between cores | Rejected |

## 2. RESP parser (`server/resp.h`)

- **Pure, incremental, no Asio dependency.** `parse(buffer)` returns `Complete` (with the number of
  bytes consumed and a `Command` whose arguments are `string_view`s into the buffer),
  `NeedMore`, or `Error`. No copies until the shard op.
- **Formats:** RESP2 arrays of bulk strings (what `redis-cli` and `redis-benchmark` send) and
  inline commands (`PING\r\n`, for telnet / nc).
- **Pipelining:** the connection parses every complete command in its read buffer, appends all
  replies to one output buffer, and sends it with a single `async_write`. A trailing partial
  command is kept and compacted to the buffer front.
- **Commands:** `GET`, `SET key value` (options such as `EX`/`NX` → error), `DEL k1 [k2 ...]`
  (integer count); plus what the tools need: `PING`, `QUIT`, `COMMAND` / `COMMAND DOCS`
  (`redis-cli` on connect → empty array), `CONFIG GET` (`redis-benchmark` at startup → empty
  array).
- **Limits** (protocol error + close, like Redis): bulk length (default 64 MiB; `Request::size`
  is `uint32_t`), array length, inline line length.
- **Backpressure:** a connection does not read while a write is pending.

## 3. Keys: string → policy `Key`

- **Policy `Key` = 64-bit hash of the key bytes.** The shard stores
  `unordered_map<Key, Entry{std::string key, std::string value}>`: at most one key per id, so a
  victim id finds its entry directly and GET compares the full key.
- **Why a hash, not a per-shard counter:** `FeatureTracker` keeps history for recently evicted keys
  (D17, D20). A key evicted and SET again must get the same id, or the learned policy loses
  exactly the history it was trained on.
- **Collisions** (two strings, one 64-bit hash, in one shard; probability ≈ n²/2⁶⁵): a shard holds
  at most one string per id. A SET of the colliding string replaces the other entry
  (`on_remove` for the old, then a normal insert); dropping an entry is always legal for a
  cache. GET compares the full string, so it never returns another key's value. Tested with an
  injectable hash.

## 4. Locking and policy isolation

- **One `std::mutex` per shard, held only for the in-memory op.** Parsing, socket I/O and reply
  formatting happen outside it.
- **Not `shared_mutex`:** a GET hit calls `policy.on_hit()`, which mutates recency/frequency state,
  so every op is a writer.
- **GET copies the value into the connection's output buffer under the lock.** Rejected for v1:
  `shared_ptr<const std::string>` values (no copy, but an atomic refcount per hit). Revisit if
  `-d 1024` profiles show the copy.
- **One policy per shard**, built by `make_policy(name, config)` with
  `capacity_bytes = total / S`. Policies with `needs_next_access()` are rejected at startup.
  `policy/`, `ml/`, `sketch/` have no global mutable or `thread_local` state (checked by grep);
  the TSan stress test is the proof.

## 5. Eviction under the shard lock

Same rules as `sim::Cache` (D2: insert, then evict):

```
SET k v:  lock(shard)
            if k present: replace value, adjust used_bytes, policy.on_hit(r)
            else:         insert, used_bytes += charge(k, v), policy.on_insert(r)
            while used_bytes > shard_capacity:
              victim = policy.evict()          // may be k itself (D2): still +OK,
              erase(victim); used_bytes -= ... //   the cache declined to keep it
          unlock -> +OK
GET k:    lock -> hit: policy.on_hit(r), copy value | miss: nothing -> unlock -> reply
DEL k...: per key: lock its shard -> erase + policy.on_remove(id) -> unlock -> :count
```

- `charge = key.size() + value.size()`. A value larger than a shard's capacity gets an error reply
  (the D2 "never inserted" rule).
- The guarded policy's time budget (D21) now also bounds shard lock hold time, so it directly
  limits p99.
- **Difference from the simulator:** a GET miss does not insert; inserts come from SET
  (cache-aside). A policy's virtual time (D11) therefore counts hits and SETs, not every request,
  which is a train/serve shift for the learned model. Measured by the replay check in section 6.
- A SET that changes a key's size calls `on_hit`; the policy's size feature keeps the first size,
  as in the simulator (D3). Known limitation.

## 6. Verification and benchmarks

**Correctness**
1. Parser unit tests: every valid input split at every byte boundary, pipelined batches, each
   limit and error path, inline commands.
2. Shard / store unit tests with LRU (deterministic order): GET/SET/DEL semantics, byte accounting,
   newcomer as victim, oversize value, forced hash collision, DEL of a missing key.
3. Concurrency stress test: T threads × random GET/SET/DEL on one store; check
   `used_bytes <= capacity` per shard and that every GET returns a value some thread wrote for
   that key.
4. In-process integration test: server on an ephemeral port, raw socket client, pipelined and
   split writes.
5. TSan (preset `tsan` under `setarch -R`, D5) on all of the above, plus the TSan server binary
   driven by `redis-benchmark -c 50 --threads 4`. ASan/UBSan likewise.

**Throughput and latency** (`redis-benchmark`, CSV output and latency percentiles)
- `redis-benchmark -t get,set -n 2000000 -r 1000000 -c {1,50,200} -P {1,16} -d {32,1024}
  --threads 4 --csv`, server threads N ∈ {1, 2, 4, 8}, policies {lru, wtinylfu, guarded}.
- Pinning with `taskset`: server on cores 0–7, client on 8–15. Record machine and WSL2.
- Baseline: `redis-server` on the same machine, `maxmemory` + `allkeys-lru`, same commands.
  Redis is single-threaded, so compare at N = 1 and at the best N, and say so.
- Report ops/sec, p50 / p99 / p99.9. Only measured numbers go into the README.

**Hit rate through the server**
- `redis-benchmark` keys are uniform random, so its hit rate says nothing about policies.
  `lcache-replay` replays a trace over RESP (GET, SET on miss).
- Check: with S = 1, the server's hit rate equals the simulator's under cache-aside semantics.
- With S = 64, the gap is the measured cost of sharding per policy (expected largest for the
  learned policy: each shard is 1/64 of the size it was trained at; normalized features, D20,
  should limit it).

## 7. Steps (one or more commits each)

1. Build: standalone Asio, `server/` target
2. RESP parser: interface + tests, then implementation
3. `Shard` / `ShardedStore`: interface + tests, then implementation
4. Connection, server, `lcache-server` app, integration test
5. Concurrency stress test, TSan run
6. `lcache-replay`, S = 1 equivalence check against the simulator
7. Benchmark scripts, `docs/m5-results.md`
