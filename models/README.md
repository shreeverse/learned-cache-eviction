# Trained models

LightGBM text models for `cachesim -p learned --model <file>`. Each must be run with the
`--history` value its training data was exported with (decisions D17); it's in the file name.

| file | trees × leaves | trained on | history |
|---|---|---|---|
| `twitter26_1MiB_h10k_32x31.txt` | 32 × 31 | Twitter cluster26, requests 0–5M, 1 MiB | 10,000 |
| `twitter26_1MiB_h10k_128x63.txt` | 128 × 63 | same | 10,000 |

Both use the same training data: sampled-LRU collector + random collector, every 50th eviction
(decisions D18). They are `reg_lr` and `big` in docs/m3-results.md. Reproduce with:

```
tools/fetch_traces.sh
T=data/twitter_cluster26_10M.oracleGeneral.bin
build/release/apps/export/lcache-export -t $T -c 1MiB -n 5000000 --every 50 --history 10000 -o lru.csv
build/release/apps/export/lcache-export -t $T -c 1MiB -n 5000000 --every 50 --history 10000 \
    --collector random -o random.csv
.venv/bin/python tools/train.py lru.csv random.csv --window 5000000 -o twitter26_1MiB_h10k_32x31.txt
.venv/bin/python tools/train.py lru.csv random.csv --window 5000000 --num-trees 128 --num-leaves 63 \
    -o twitter26_1MiB_h10k_128x63.txt
```
