// Counting libc allocator shared by heap_probe and voice_alloc_check.
// Include from exactly one translation unit per executable. Replaces the
// malloc family for the whole process (glibc), tracks live/peak bytes, and
// can abort() on any allocation made while the calling thread has
// tlInRender set (the audio-thread no-allocation proof).
#pragma once
#include <atomic>
#include <execinfo.h>
#include <malloc.h>
#include <unistd.h>
#include <cstdlib>
#include <cstdio>

// ---------------------------------------------------------------------------
// Counting allocator (glibc). Replaces malloc family for the whole process.
// ---------------------------------------------------------------------------
extern "C" {
void* __libc_malloc(size_t);
void* __libc_calloc(size_t, size_t);
void* __libc_realloc(void*, size_t);
void __libc_free(void*);
void* __libc_memalign(size_t, size_t);
}
namespace {
std::atomic<long long> gLive{0}, gPeak{0};
std::atomic<long long> gAllocCalls{0};
thread_local int tlInRender = 0;
std::atomic<long long> gRenderAllocs{0};
bool gTrap = false;

void noteAlloc(void* p) {
  if (!p) return;
  long long n = (long long)malloc_usable_size(p);
  long long live = gLive.fetch_add(n, std::memory_order_relaxed) + n;
  long long peak = gPeak.load(std::memory_order_relaxed);
  while (live > peak && !gPeak.compare_exchange_weak(peak, live)) {}
  gAllocCalls.fetch_add(1, std::memory_order_relaxed);
  if (tlInRender) {
    gRenderAllocs.fetch_add(1, std::memory_order_relaxed);
    if (gTrap) {
      const char msg[] = "heap_probe: ALLOCATION ON AUDIO THREAD\n";
      if (write(2, msg, sizeof msg - 1)) {}
      tlInRender = 0;
      void* frames[40];
      int n = backtrace(frames, 40);
      backtrace_symbols_fd(frames, n, 2);
      abort();
    }
  }
}
void noteFree(void* p) {
  if (p) gLive.fetch_sub((long long)malloc_usable_size(p), std::memory_order_relaxed);
}
}  // namespace

#ifndef PROBE_NO_MALLOC
extern "C" {
void* malloc(size_t n) { void* p = __libc_malloc(n); noteAlloc(p); return p; }
void* calloc(size_t a, size_t b) { void* p = __libc_calloc(a, b); noteAlloc(p); return p; }
void* realloc(void* old, size_t n) {
  noteFree(old);
  void* p = __libc_realloc(old, n);
  if (p) noteAlloc(p); else if (old) noteAlloc(old);
  return p;
}
void free(void* p) { noteFree(p); __libc_free(p); }
void* memalign(size_t al, size_t n) { void* p = __libc_memalign(al, n); noteAlloc(p); return p; }
void* aligned_alloc(size_t al, size_t n) { return memalign(al, n); }
int posix_memalign(void** out, size_t al, size_t n) {
  void* p = memalign(al, n);
  if (!p) return 12;
  *out = p;
  return 0;
}
}
#endif

