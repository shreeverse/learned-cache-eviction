// Prints exact hit counts from libCacheSim, in the same CSV format as our cachesim.
//
// libCacheSim's own CLI only prints miss ratios to 4 decimals (%.4lf), which can't show an exact
// match. This drives its library API directly: same trace reader, same policy, raw counters.
//
// Usage: libcachesim_exact TRACE.oracleGeneral.bin ALGO IGNORE_SIZE(0|1) SIZE [SIZE...]
//   ALGO: lru | lfu | arc | wtinylfu | belady (libCacheSim defaults for each)
// Built by tools/validate_vs_libcachesim.sh.

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libCacheSim.h"

static cache_t* make_cache(const char* algo, common_cache_params_t params) {
  if (strcmp(algo, "lru") == 0) return LRU_init(params, NULL);
  if (strcmp(algo, "lfu") == 0) return LFU_init(params, NULL);
  if (strcmp(algo, "arc") == 0) return ARC_init(params, NULL);
  if (strcmp(algo, "wtinylfu") == 0) return WTinyLFU_init(params, NULL);
  if (strcmp(algo, "belady") == 0) return Belady_init(params, NULL);
  return NULL;
}

int main(int argc, char** argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: %s TRACE ALGO IGNORE_SIZE(0|1) SIZE [SIZE...]\n", argv[0]);
    return 2;
  }
  const char* path = argv[1];
  const char* algo = argv[2];
  const int ignore_size = atoi(argv[3]);

  printf("trace,policy,cache_size,ignore_size,requests,hits,bytes_requested,bytes_hit\n");
  for (int i = 4; i < argc; i++) {
    const uint64_t size = strtoull(argv[i], NULL, 10);

    reader_init_param_t rparams = default_reader_init_params();
    rparams.ignore_obj_size = ignore_size != 0;
    reader_t* reader = open_trace(path, ORACLE_GENERAL_TRACE, &rparams);

    common_cache_params_t cparams = default_common_cache_params();
    cparams.cache_size = size;
    cache_t* cache = make_cache(algo, cparams);
    if (cache == NULL) {
      fprintf(stderr, "unknown algo %s\n", algo);
      return 2;
    }

    request_t* req = new_request();
    uint64_t n = 0, hits = 0, bytes = 0, bytes_hit = 0;
    while (read_one_req(reader, req) == 0) {
      n++;
      bytes += req->obj_size;
      if (cache->get(cache, req)) {
        hits++;
        bytes_hit += req->obj_size;
      }
    }
    printf("%s,%s,%" PRIu64 ",%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n", path, algo, size,
           ignore_size ? "true" : "false", n, hits, bytes, bytes_hit);

    free_request(req);
    cache->cache_free(cache);
    close_trace(reader);
  }
  return 0;
}
