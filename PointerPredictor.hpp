#include <SDL3/SDL_log.h>
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_timer.h>
#include <cstddef>
#include <deque>

struct PointerPredictor {
  static constexpr size_t max_samples = 100;
  std::deque<std::pair<SDL_FPoint, uint64_t>> samples;

  void reset() { samples.clear(); }

  void add_sample(SDL_FPoint pos, uint64_t timestamp) {
    // If the new sample is very close in time to the previous one
    // (e.g. multiple motion events dequeued in one PollEvent batch),
    // update the previous sample's position instead of adding a new
    // entry.  This avoids near-zero dt with large dx → inflated velocity.
    constexpr uint64_t COALESCE_NS = 500000; // 0.5 ms
    if (!samples.empty() && (timestamp - samples.back().second) < COALESCE_NS) {
      samples.back().first = pos;
      samples.back().second = timestamp;
      return;
    }
    samples.emplace_back(pos, timestamp);
    while (samples.size() > max_samples) {
      samples.pop_front();
    }
  }

  bool can_predict() const { return !samples.empty(); }

  SDL_FPoint predict(uint64_t timestamp) const;
};
