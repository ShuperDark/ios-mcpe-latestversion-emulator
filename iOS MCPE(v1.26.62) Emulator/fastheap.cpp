#include "fastheap.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

namespace fastheap {
namespace {

constexpr size_t kGranule = 16;
constexpr size_t kClasses = 32;                // 16, 32, ... 512 bytes
constexpr size_t kMaxSmall = kGranule * kClasses;
constexpr size_t kCacheBytesPerClass = 64 * 1024;
constexpr uint32_t kMagic = 0x46480000u;       // "FH" in the high half, the class in the low half

// 16 bytes in front of every block
struct Header {
    uint32_t tag;     // kMagic | class (0: a large block)
    uint32_t offset;  // user pointer - start of the malloc'd memory
    uint64_t size;    // large blocks: the requested size
};
static_assert(sizeof(Header) == 16);

struct Node { Node* next; };

// trivially destructible, so it stays usable while other thread_locals are destroyed
struct Cache {
    Node* head[kClasses];
    uint32_t count[kClasses];
    bool dead;
};
thread_local Cache t_cache;

size_t class_limit(size_t cls) {
    size_t n = kCacheBytesPerClass / (cls * kGranule);
    return n < 32 ? 32 : n;
}

void flush(Cache& c) {
    for (size_t i = 0; i < kClasses; i++) {
        Node* n = c.head[i];
        while (n) {
            Node* next = n->next;
            std::free(reinterpret_cast<char*>(n) - sizeof(Header));
            n = next;
        }
        c.head[i] = nullptr;
        c.count[i] = 0;
    }
}

// gives the cache back to the system heap when the thread exits
struct Guard {
    bool armed = false;
    ~Guard() {
        flush(t_cache);
        t_cache.dead = true;
    }
};
thread_local Guard t_guard;

Header* header_of(void* p) { return reinterpret_cast<Header*>(p) - 1; }

// (constructs the guard on first use, so the thread's cache is given back when it exits)
inline void arm(Cache& c) {
    if (!c.dead && !t_guard.armed) t_guard.armed = true;
}

void* alloc_large(size_t n, size_t align) {
    if (align < kGranule) align = kGranule;
    if (n > SIZE_MAX - align - sizeof(Header)) return nullptr;
    char* raw = static_cast<char*>(std::malloc(n + sizeof(Header) + (align > kGranule ? align : 0)));
    if (!raw) return nullptr;
    uintptr_t user = reinterpret_cast<uintptr_t>(raw) + sizeof(Header);
    user = (user + align - 1) & ~(uintptr_t)(align - 1);
    Header* h = reinterpret_cast<Header*>(user) - 1;
    h->tag = kMagic;
    h->offset = static_cast<uint32_t>(user - reinterpret_cast<uintptr_t>(raw));
    h->size = n;
    return reinterpret_cast<void*>(user);
}

}  // namespace

void* alloc(size_t n) {
    if (n == 0) n = 1;
    if (n > kMaxSmall) return alloc_large(n, kGranule);
    const size_t cls = (n + kGranule - 1) / kGranule;  // 1..kClasses
    Cache& c = t_cache;
    if (Node* f = c.head[cls - 1]) {
        c.head[cls - 1] = f->next;
        c.count[cls - 1]--;
        return f;
    }
    arm(c);
    char* raw = static_cast<char*>(std::malloc(sizeof(Header) + cls * kGranule));
    if (!raw) return nullptr;
    Header* h = reinterpret_cast<Header*>(raw);
    h->tag = kMagic | static_cast<uint32_t>(cls);
    h->offset = sizeof(Header);
    h->size = 0;
    return h + 1;
}

void* alloc_aligned(size_t n, size_t align) {
    if (align <= kGranule) return alloc(n);
    return alloc_large(n ? n : 1, align);
}

void free(void* p) {
    if (!p) return;
    Header* h = header_of(p);
    const size_t cls = h->tag & 0xFFFF;
    if (cls) {
        Cache& c = t_cache;
        if (!c.dead && c.count[cls - 1] < class_limit(cls)) {
            arm(c);
            Node* n = static_cast<Node*>(p);
            n->next = c.head[cls - 1];
            c.head[cls - 1] = n;
            c.count[cls - 1]++;
            return;
        }
    }
    std::free(static_cast<char*>(p) - h->offset);
}

size_t usable_size(void* p) {
    if (!p) return 0;
    Header* h = header_of(p);
    const size_t cls = h->tag & 0xFFFF;
    return cls ? cls * kGranule : static_cast<size_t>(h->size);
}

void* realloc(void* p, size_t n) {
    if (!p) return alloc(n);
    if (n == 0) { free(p); return nullptr; }
    const size_t old = usable_size(p);
    Header* h = header_of(p);
    // a small block that still fits (and is not much too large) stays where it is
    if ((h->tag & 0xFFFF) && n <= old && n > old / 2) return p;
    // a large block that stays large: the system realloc (it can often grow in place); the
    // header moves with the block (only for the default alignment: the header right at the start)
    if (!(h->tag & 0xFFFF) && n > kMaxSmall && h->offset == sizeof(Header) && n <= SIZE_MAX - sizeof(Header)) {
        char* raw = static_cast<char*>(std::realloc(h, n + sizeof(Header)));
        if (!raw) return nullptr;
        Header* nh = reinterpret_cast<Header*>(raw);
        nh->size = n;
        return nh + 1;
    }
    void* q = alloc(n);
    if (!q) return nullptr;
    std::memcpy(q, p, old < n ? old : n);
    free(p);
    return q;
}

}  // namespace fastheap

// ---- the emulator's operator new / delete ----
void* operator new(size_t n) {
    if (void* p = fastheap::alloc(n)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t n) {
    if (void* p = fastheap::alloc(n)) return p;
    throw std::bad_alloc();
}
void* operator new(size_t n, const std::nothrow_t&) noexcept { return fastheap::alloc(n); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return fastheap::alloc(n); }
void* operator new(size_t n, std::align_val_t a) {
    if (void* p = fastheap::alloc_aligned(n, static_cast<size_t>(a))) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t n, std::align_val_t a) {
    if (void* p = fastheap::alloc_aligned(n, static_cast<size_t>(a))) return p;
    throw std::bad_alloc();
}
void* operator new(size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return fastheap::alloc_aligned(n, static_cast<size_t>(a));
}
void* operator new[](size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return fastheap::alloc_aligned(n, static_cast<size_t>(a));
}
void operator delete(void* p) noexcept { fastheap::free(p); }
void operator delete[](void* p) noexcept { fastheap::free(p); }
void operator delete(void* p, size_t) noexcept { fastheap::free(p); }
void operator delete[](void* p, size_t) noexcept { fastheap::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { fastheap::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { fastheap::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { fastheap::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { fastheap::free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { fastheap::free(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { fastheap::free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { fastheap::free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { fastheap::free(p); }
