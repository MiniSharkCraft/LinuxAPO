#pragma once

#include <cstddef>
#include <limits>
#include <new>

namespace skyapo::testing {

inline std::size_t &pdcAllocationFailureCountdown() noexcept {
  thread_local std::size_t countdown = std::numeric_limits<std::size_t>::max();
  return countdown;
}

inline std::size_t &successfulPdcRingAllocations() noexcept {
  thread_local std::size_t successful = 0;
  return successful;
}

inline void
failPdcAllocationAfterForTesting(std::size_t successfulAllocations) noexcept {
  pdcAllocationFailureCountdown() = successfulAllocations;
  successfulPdcRingAllocations() = 0;
}

inline void clearPdcAllocationFailureForTesting() noexcept {
  pdcAllocationFailureCountdown() = std::numeric_limits<std::size_t>::max();
}

inline void beforePdcRingAllocationForTesting() {
  auto &countdown = pdcAllocationFailureCountdown();
  if (countdown == std::numeric_limits<std::size_t>::max())
    return;
  if (countdown == 0) {
    clearPdcAllocationFailureForTesting();
    throw std::bad_alloc();
  }
  --countdown;
}

inline void recordPdcRingAllocationForTesting(std::size_t samples) noexcept {
  if (samples != 0 && pdcAllocationFailureCountdown() !=
                          std::numeric_limits<std::size_t>::max())
    ++successfulPdcRingAllocations();
}

inline std::size_t successfulPdcRingAllocationsForTesting() noexcept {
  return successfulPdcRingAllocations();
}

} // namespace skyapo::testing
