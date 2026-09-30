#pragma once

#include <atomic>
#include <cstdint>

namespace skyapo::pipewire {

// Associates deferred event-loop telemetry work with the graph generation
// that produced it. Audio-thread completion is published atomically; the
// reset-generation field is intentionally event-loop-thread-only.
class TransitionMetricsGeneration {
public:
  uint64_t graphReplaced() noexcept {
    return generation.fetch_add(1, std::memory_order_seq_cst) + 1;
  }

  void transitionStarted(uint64_t id) noexcept {
    pending.store(id, std::memory_order_seq_cst);
  }

  void transitionCompleted() noexcept {
    completed.store(pending.load(std::memory_order_seq_cst),
                    std::memory_order_seq_cst);
  }

  bool shouldResetMetrics() noexcept {
    const auto done = completed.load(std::memory_order_seq_cst);
    const auto current = generation.load(std::memory_order_seq_cst);
    if (!done || done != current || done == resetGeneration)
      return false;
    resetGeneration = done;
    return true;
  }

private:
  std::atomic<uint64_t> generation{0};
  std::atomic<uint64_t> pending{0};
  std::atomic<uint64_t> completed{0};
  uint64_t resetGeneration{};
};

} // namespace skyapo::pipewire
