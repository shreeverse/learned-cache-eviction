#include "api/visualize.h"

#include <algorithm>
#include <stdexcept>

namespace lcache::api {

void SlotMap::apply(const RecordingPolicy& rec) {
  const auto& victims = rec.evicted();
  const auto inserted = rec.inserted();
  // Victims leave first: the cache inserts, then evicts (D2), so with a full map the newcomer
  // only fits once a victim's slot is free. A newcomer that is its own victim never had a slot.
  for (const Key v : victims) {
    if (v != inserted) remove(v);
  }
  if (inserted && std::find(victims.begin(), victims.end(), *inserted) == victims.end()) {
    add(*inserted);
  }
}

std::optional<std::size_t> SlotMap::find(Key k) const {
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    if (slots_[i] == k) return i;
  }
  return std::nullopt;
}

void SlotMap::add(Key k) {
  const auto free = std::find(slots_.begin(), slots_.end(), std::nullopt);
  if (free == slots_.end()) throw std::logic_error("SlotMap: no free slot for an inserted key");
  *free = k;
}

void SlotMap::remove(Key k) {
  const auto slot = find(k);
  if (!slot) throw std::logic_error("SlotMap: evicted key has no slot");
  slots_[*slot].reset();
}

std::string key_label(std::size_t index) {
  std::string label;
  for (std::size_t n = index + 1; n > 0; n = (n - 1) / 26) {
    label.insert(label.begin(), static_cast<char>('A' + (n - 1) % 26));
  }
  return label;
}

}  // namespace lcache::api
