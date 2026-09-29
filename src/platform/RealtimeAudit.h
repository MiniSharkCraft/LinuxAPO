#pragma once
#include <atomic>
#include <cstdint>
namespace realtime {
extern thread_local bool inCallback;
extern std::atomic<uint64_t> allocations, deallocations;
struct Scope {
  Scope() { inCallback = true; }
  ~Scope() { inCallback = false; }
};
} // namespace realtime
