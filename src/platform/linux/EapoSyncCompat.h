#pragma once

// Narrow POSIX compatibility layer for the upstream LoudnessCorrection
// filter's worker thread and event hand-off. Realtime code only polls a
// preallocated atomic event with timeout 0; it never waits on a condition
// variable or takes the control-thread mutex.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

#define __stdcall

using HANDLE = void *;
using HRESULT = long;
using EapoThreadStart = unsigned long (*)(void *);

inline constexpr HRESULT S_OK = 0;
inline constexpr HRESULT S_FALSE = 1;
inline constexpr HRESULT E_NOTIMPL = static_cast<HRESULT>(0x80004001L);
inline constexpr unsigned long WAIT_OBJECT_0 = 0;
inline constexpr unsigned long WAIT_TIMEOUT = 258;
inline constexpr unsigned long WAIT_FAILED = 0xffffffffUL;
inline constexpr unsigned long INFINITE = 0xffffffffUL;

struct CRITICAL_SECTION {
  std::mutex mutex;
};

inline void InitializeCriticalSection(CRITICAL_SECTION *) noexcept {}
inline void DeleteCriticalSection(CRITICAL_SECTION *) noexcept {}
inline void EnterCriticalSection(CRITICAL_SECTION *section) noexcept {
  section->mutex.lock();
}
inline bool TryEnterCriticalSection(CRITICAL_SECTION *section) noexcept {
  return section->mutex.try_lock();
}
inline void LeaveCriticalSection(CRITICAL_SECTION *section) noexcept {
  section->mutex.unlock();
}

namespace skyapo::platform {
struct EapoHandle {
  enum class Kind { Event, Thread };
  explicit EapoHandle(Kind kind) : kind(kind) {}
  virtual ~EapoHandle() = default;
  Kind kind;
};

struct EapoEvent final : EapoHandle {
  explicit EapoEvent(bool initial)
      : EapoHandle(Kind::Event), signaled(initial) {}
  std::atomic<bool> signaled;
};

struct EapoThread final : EapoHandle {
  explicit EapoThread(std::thread thread)
      : EapoHandle(Kind::Thread), thread(std::move(thread)) {}
  std::thread thread;
};
} // namespace skyapo::platform

inline HANDLE CreateEvent(void *, bool, bool initialState, const wchar_t *) {
  return new (std::nothrow) skyapo::platform::EapoEvent(initialState);
}

inline HANDLE CreateThread(void *, std::size_t, EapoThreadStart start,
                           void *parameter, unsigned long,
                           unsigned long *) {
  try {
    return new skyapo::platform::EapoThread(
        std::thread([start, parameter] { start(parameter); }));
  } catch (...) {
    return nullptr;
  }
}

inline unsigned long WaitForSingleObject(HANDLE handle,
                                         unsigned long milliseconds) {
  if (!handle)
    return WAIT_FAILED;
  auto *base = static_cast<skyapo::platform::EapoHandle *>(handle);
  if (base->kind == skyapo::platform::EapoHandle::Kind::Event) {
    if (milliseconds != 0)
      return WAIT_FAILED;
    const auto *event = static_cast<skyapo::platform::EapoEvent *>(base);
    return event->signaled.load(std::memory_order_acquire) ? WAIT_OBJECT_0
                                                           : WAIT_TIMEOUT;
  }
  if (milliseconds != INFINITE)
    return WAIT_FAILED;
  auto *thread = static_cast<skyapo::platform::EapoThread *>(base);
  if (thread->thread.joinable())
    thread->thread.join();
  return WAIT_OBJECT_0;
}

inline bool SetEvent(HANDLE handle) noexcept {
  if (!handle)
    return false;
  auto *base = static_cast<skyapo::platform::EapoHandle *>(handle);
  if (base->kind != skyapo::platform::EapoHandle::Kind::Event)
    return false;
  static_cast<skyapo::platform::EapoEvent *>(base)->signaled.store(
      true, std::memory_order_release);
  return true;
}

inline bool ResetEvent(HANDLE handle) noexcept {
  if (!handle)
    return false;
  auto *base = static_cast<skyapo::platform::EapoHandle *>(handle);
  if (base->kind != skyapo::platform::EapoHandle::Kind::Event)
    return false;
  static_cast<skyapo::platform::EapoEvent *>(base)->signaled.store(
      false, std::memory_order_release);
  return true;
}

inline bool CloseHandle(HANDLE handle) noexcept {
  delete static_cast<skyapo::platform::EapoHandle *>(handle);
  return true;
}

inline void Sleep(unsigned long milliseconds) {
  std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}
