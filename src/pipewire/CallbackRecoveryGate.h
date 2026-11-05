#pragma once

#include <atomic>

namespace skyapo::pipewire::detail {

static_assert(std::atomic<unsigned>::is_always_lock_free,
              "realtime callback admission requires lock-free counters");
static_assert(std::atomic<bool>::is_always_lock_free,
              "realtime callback admission requires lock-free flags");

// Admission boundary for realtime callbacks while the control thread replaces
// an Engine after a fault. A callback must count itself before checking the
// closed flag. Together with sequentially-consistent ordering, this ensures a
// callback either observes the closed gate and avoids Engine access, or is
// already visible to the control thread's drain before the gate is closed.
class CallbackRecoveryGate {
public:
  class Flight {
  public:
    explicit Flight(CallbackRecoveryGate &gate) noexcept : gate_(gate) {
      gate_.callbacksInFlight_.fetch_add(1, std::memory_order_seq_cst);
      mayProcess_ = !gate_.closed_.load(std::memory_order_seq_cst);
    }

    Flight(const Flight &) = delete;
    Flight &operator=(const Flight &) = delete;
    Flight(Flight &&) = delete;
    Flight &operator=(Flight &&) = delete;

    ~Flight() {
      gate_.callbacksInFlight_.fetch_sub(1, std::memory_order_seq_cst);
    }

    bool mayProcess() const noexcept {
      return mayProcess_;
    }

  private:
    CallbackRecoveryGate &gate_;
    bool mayProcess_{};
  };

  [[nodiscard]] Flight enterCallback() noexcept {
    return Flight(*this);
  }

  void closeForRecovery() noexcept {
    closed_.store(true, std::memory_order_seq_cst);
  }

  void finishRecovery() noexcept {
    closed_.store(false, std::memory_order_seq_cst);
  }

  bool recoveryInProgress() const noexcept {
    return closed_.load(std::memory_order_seq_cst);
  }

  unsigned callbacksInFlight() const noexcept {
    return callbacksInFlight_.load(std::memory_order_seq_cst);
  }

private:
  std::atomic<unsigned> callbacksInFlight_{0};
  std::atomic<bool> closed_{false};
};

} // namespace skyapo::pipewire::detail
