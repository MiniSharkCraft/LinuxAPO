#include "RealtimeAudit.h"
#include <cstdlib>
#include <new>
namespace realtime {
thread_local bool inCallback = false;
std::atomic<uint64_t> allocations{0}, deallocations{0};
} // namespace realtime
extern "C" {
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
int __real_posix_memalign(void **, size_t, size_t);
void *__wrap_malloc(size_t n) {
  if (realtime::inCallback)
    ++realtime::allocations;
  return __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t s) {
  if (realtime::inCallback)
    ++realtime::allocations;
  return __real_calloc(n, s);
}
void *__wrap_realloc(void *p, size_t n) {
  if (realtime::inCallback)
    ++realtime::allocations;
  return __real_realloc(p, n);
}
void __wrap_free(void *p) {
  if (p && realtime::inCallback)
    ++realtime::deallocations;
  __real_free(p);
}
int __wrap_posix_memalign(void **p, size_t a, size_t n) {
  if (realtime::inCallback)
    ++realtime::allocations;
  return __real_posix_memalign(p, a, n);
}
}
void *operator new(size_t n) {
  if (realtime::inCallback)
    ++realtime::allocations;
  if (auto *p = __real_malloc(n ? n : 1))
    return p;
  throw std::bad_alloc();
}
void *operator new[](size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept {
  if (p && realtime::inCallback)
    ++realtime::deallocations;
  __real_free(p);
}
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p, size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p, size_t) noexcept { ::operator delete(p); }
void *operator new(size_t n, std::align_val_t a) {
  if (realtime::inCallback)
    ++realtime::allocations;
  void *p = nullptr;
  if (__real_posix_memalign(&p, static_cast<size_t>(a), n ? n : 1) != 0)
    throw std::bad_alloc();
  return p;
}
void *operator new[](size_t n, std::align_val_t a) {
  return ::operator new(n, a);
}
void operator delete(void *p, std::align_val_t) noexcept {
  ::operator delete(p);
}
void operator delete[](void *p, std::align_val_t) noexcept {
  ::operator delete(p);
}
void operator delete(void *p, size_t, std::align_val_t) noexcept {
  ::operator delete(p);
}
void operator delete[](void *p, size_t, std::align_val_t) noexcept {
  ::operator delete(p);
}
void *operator new(size_t n, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(n);
  } catch (...) {
    return nullptr;
  }
}
void *operator new[](size_t n, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(n);
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void *p, const std::nothrow_t &) noexcept {
  ::operator delete(p);
}
void operator delete[](void *p, const std::nothrow_t &) noexcept {
  ::operator delete(p);
}
