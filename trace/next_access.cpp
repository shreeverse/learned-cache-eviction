#include "trace/next_access.h"

#include <unordered_map>

namespace lcache {

std::vector<Request> annotate_next_access(TraceReader& reader, std::uint64_t max_requests) {
  reader.reset();
  std::vector<Request> reqs;
  Request r;
  while ((max_requests == 0 || reqs.size() < max_requests) && reader.next(r)) {
    reqs.push_back(r);
  }

  // Walk backwards: the last time we saw a key (scanning from the end) is its next access.
  std::unordered_map<Key, std::uint64_t> seen;
  for (std::uint64_t i = reqs.size(); i-- > 0;) {
    auto [it, inserted] = seen.try_emplace(reqs[i].key, i);
    reqs[i].next_vtime = inserted ? kNever : it->second;
    it->second = i;
  }
  return reqs;
}

}  // namespace lcache
