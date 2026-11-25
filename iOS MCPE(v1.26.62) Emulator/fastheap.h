// Small-object allocator with per-thread caches, for the emulator's own operator new/delete
// and for the guest's malloc/free (vmem::gmalloc...).
//
// The JIT allocates and frees many small objects for every block it compiles, and the game
// calls malloc/free hundreds of thousands of times per second; the Windows heap took about
// a tenth of the compile time. Blocks up to 512 bytes come from (and go back to) a cache of
// the calling thread; larger ones go to malloc. Every block has a 16-byte header, so user
// pointers are 16-byte aligned and the usable size is known (malloc_size).
#pragma once
#include <cstddef>

namespace fastheap {
void* alloc(size_t n);                       // 16-byte aligned; nullptr when out of memory
void* alloc_aligned(size_t n, size_t align); // align: a power of two
void* realloc(void* p, size_t n);
void free(void* p);
size_t usable_size(void* p);
}  // namespace fastheap
