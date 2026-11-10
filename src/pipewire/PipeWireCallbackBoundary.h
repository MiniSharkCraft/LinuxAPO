#pragma once

#include <atomic>
#include <utility>

namespace skyapo::pipewire::detail {

template <typename Callback>
void invokeControlCallback(Callback &&callback,
                           std::atomic<bool> &callbackFailed,
                           std::atomic<bool> &faulted) noexcept {
  try {
    std::forward<Callback>(callback)();
  } catch (...) {
    callbackFailed.store(true, std::memory_order_release);
    faulted.store(true, std::memory_order_release);
  }
}

} // namespace skyapo::pipewire::detail
