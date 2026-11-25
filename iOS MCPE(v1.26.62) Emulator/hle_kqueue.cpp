// kqueue / kevent (libSystem) for the guest's event loops - asio (the web sockets of the
// HTTP library, used once signed in to Xbox Live) runs on kqueue on Apple platforms.
//
// Readiness is polled: sockets with WSAPoll, pipes with PeekNamedPipe, EVFILT_USER and
// EVFILT_TIMER in memory, every millisecond while kevent waits. EV_CLEAR (edge
// triggered, what asio asks for) is reported when the state changes and again every
// 50 ms while it stays ready, so a change between two checks is never lost and a ready
// socket does not wake the loop continuously.
#include "fdtab.h"
#include "hle_common.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>

#include <map>
#include <mutex>
#include <vector>

using namespace hlec;

namespace {

constexpr s16 EVFILT_READ = -1, EVFILT_WRITE = -2, EVFILT_TIMER = -7, EVFILT_USER = -10;
constexpr u16 EV_ADD = 0x1, EV_DELETE = 0x2, EV_ENABLE = 0x4, EV_DISABLE = 0x8, EV_ONESHOT = 0x10, EV_CLEAR = 0x20,
              EV_RECEIPT = 0x40, EV_DISPATCH = 0x80, EV_ERROR = 0x4000, EV_EOF = 0x8000;
constexpr u32 NOTE_TRIGGER = 0x01000000, NOTE_FFCTRLMASK = 0xc0000000, NOTE_FFLAGSMASK = 0x00ffffff;
constexpr u32 NOTE_SECONDS = 0x1, NOTE_USECONDS = 0x2, NOTE_NSECONDS = 0x4;

#pragma pack(push, 1)
struct DKevent {
    u64 ident;
    s16 filter;
    u16 flags;
    u32 fflags;
    s64 data;
    u64 udata;
};
struct DKevent64 {
    u64 ident;
    s16 filter;
    u16 flags;
    u32 fflags;
    s64 data;
    u64 udata;
    u64 ext[2];
};
#pragma pack(pop)
static_assert(sizeof(DKevent) == 32 && sizeof(DKevent64) == 48);

struct Reg {
    u16 flags = 0;
    u32 fflags = 0;
    s64 data = 0;
    u64 udata = 0;
    bool enabled = true;
    bool triggered = false;  // EVFILT_USER
    bool last_ready = false; // EV_CLEAR: the state at the last check
    ULONGLONG last_report = 0;
    ULONGLONG next_fire = 0; // EVFILT_TIMER
};

struct Kq {
    std::mutex m;
    std::map<std::pair<u64, s16>, Reg> regs;
};

std::shared_ptr<Kq> get_kq(int fd) {
    auto f = fdtab::get(fd);
    if (!f || f->kind != fdtab::Kind::Kqueue) return nullptr;
    return std::static_pointer_cast<Kq>(f->kq);
}

ULONGLONG timer_ms(const Reg& r) {
    s64 v = r.data;
    if (r.fflags & NOTE_SECONDS) return (ULONGLONG)v * 1000;
    if (r.fflags & NOTE_USECONDS) return (ULONGLONG)(v / 1000);
    if (r.fflags & NOTE_NSECONDS) return (ULONGLONG)(v / 1000000);
    return (ULONGLONG)v;  // milliseconds
}

// applies one change; returns 0 or a Darwin errno
int apply(Kq& kq, const DKevent& c) {
    auto key = std::make_pair(c.ident, c.filter);
    auto it = kq.regs.find(key);
    if (c.flags & EV_DELETE) {
        if (it == kq.regs.end()) return D_ENOENT;
        kq.regs.erase(it);
        return 0;
    }
    if (c.filter != EVFILT_READ && c.filter != EVFILT_WRITE && c.filter != EVFILT_TIMER && c.filter != EVFILT_USER) return D_EINVAL;
    if ((c.filter == EVFILT_READ || c.filter == EVFILT_WRITE) && !fdtab::get((int)c.ident)) return D_EBADF;
    if (it == kq.regs.end()) {
        if (!(c.flags & EV_ADD)) return D_ENOENT;
        it = kq.regs.emplace(key, Reg{}).first;
    }
    Reg& r = it->second;
    if (c.flags & EV_ADD) {
        r.flags = c.flags & (EV_ONESHOT | EV_CLEAR | EV_DISPATCH);
        r.udata = c.udata;
        r.enabled = true;
        r.last_ready = false;
        r.last_report = 0;
        if (c.filter == EVFILT_TIMER) {
            r.data = c.data;
            r.fflags = c.fflags;
            r.next_fire = GetTickCount64() + timer_ms(r);
        }
    }
    if (c.flags & EV_ENABLE) r.enabled = true;
    if (c.flags & EV_DISABLE) r.enabled = false;
    if (c.filter == EVFILT_USER) {
        u32 ctrl = c.fflags & NOTE_FFCTRLMASK, ff = c.fflags & NOTE_FFLAGSMASK;
        if (ctrl == 0x40000000) r.fflags &= ff;        // NOTE_FFAND
        else if (ctrl == 0x80000000) r.fflags |= ff;   // NOTE_FFOR
        else if (ctrl == 0xc0000000) r.fflags = ff;    // NOTE_FFCOPY
        if (c.fflags & NOTE_TRIGGER) r.triggered = true;
        if (c.flags & EV_ADD) r.udata = c.udata;
    }
    return 0;
}

// one pass over the registrations: fills out (up to max), returns the count
int collect(Kq& kq, DKevent* out, int max) {
    ULONGLONG now = GetTickCount64();
    // sockets in one WSAPoll
    std::vector<WSAPOLLFD> polls;
    std::vector<Reg*> sock_regs;
    std::vector<std::pair<u64, s16>> sock_keys;
    for (auto& [key, r] : kq.regs) {
        if (!r.enabled || (key.second != EVFILT_READ && key.second != EVFILT_WRITE)) continue;
        auto f = fdtab::get((int)key.first);
        if (!f || f->kind != fdtab::Kind::Socket) continue;
        WSAPOLLFD p{};
        p.fd = (SOCKET)f->sock;
        p.events = key.second == EVFILT_READ ? POLLRDNORM : POLLWRNORM;
        polls.push_back(p);
        sock_regs.push_back(&r);
        sock_keys.push_back(key);
    }
    if (!polls.empty()) WSAPoll(polls.data(), (ULONG)polls.size(), 0);
    size_t si = 0;
    int n = 0;
    std::vector<std::pair<u64, s16>> remove;
    for (auto& [key, r] : kq.regs) {
        if (n >= max) break;
        if (!r.enabled) continue;
        bool ready = false, eof = false;
        s64 data = 0;
        u32 fflags = 0;
        if (key.second == EVFILT_USER) {
            ready = r.triggered;
            fflags = r.fflags;
        } else if (key.second == EVFILT_TIMER) {
            ready = now >= r.next_fire;
            if (ready) {
                ULONGLONG iv = std::max<ULONGLONG>(1, timer_ms(r));
                data = (s64)((now - r.next_fire) / iv + 1);
                r.next_fire = now + iv;
            }
        } else {
            auto f = fdtab::get((int)key.first);
            if (!f) {
                ready = true;
                eof = true;
            } else if (f->kind == fdtab::Kind::Socket) {
                // (the sockets were polled above, in the same order)
                while (si < sock_keys.size() && sock_keys[si] != key) si++;
                if (si < sock_keys.size()) {
                    short re = polls[si].revents;
                    ready = re != 0;
                    eof = (re & (POLLHUP | POLLERR)) != 0;
                    if (key.second == EVFILT_READ && (re & POLLRDNORM)) {
                        u_long avail = 0;
                        ioctlsocket((SOCKET)f->sock, FIONREAD, &avail);
                        data = (s64)avail;
                    }
                }
            } else if (f->kind == fdtab::Kind::PipeRead && key.second == EVFILT_READ) {
                DWORD avail = 0;
                if (!PeekNamedPipe((HANDLE)f->handle, nullptr, 0, nullptr, &avail, nullptr)) ready = eof = true;
                else if (avail) { ready = true; data = avail; }
            } else {
                ready = true;  // files, pipes for writing: always ready
            }
        }
        bool report = ready;
        if (ready && (r.flags & EV_CLEAR) && key.second != EVFILT_USER && key.second != EVFILT_TIMER)
            report = !r.last_ready || now - r.last_report >= 50;
        r.last_ready = ready;
        if (!report) continue;
        DKevent& e = out[n++];
        e.ident = key.first;
        e.filter = key.second;
        e.flags = (u16)((eof ? EV_EOF : 0) | (r.flags & (EV_ONESHOT | EV_CLEAR | EV_DISPATCH)));
        e.fflags = fflags;
        e.data = data;
        e.udata = r.udata;
        r.last_report = now;
        if (key.second == EVFILT_USER && (r.flags & EV_CLEAR)) r.triggered = false;
        if (r.flags & EV_ONESHOT) remove.push_back(key);
        else if (r.flags & EV_DISPATCH) r.enabled = false;
    }
    for (auto& k : remove) kq.regs.erase(k);
    return n;
}

int do_kevent(int kqfd, const DKevent* changes, int nchanges, DKevent* events, int nevents, const d_timespec* ts) {
    auto kq = get_kq(kqfd);
    if (!kq) { set_errno(D_EBADF); return -1; }
    int n = 0;
    {
        std::lock_guard lk(kq->m);
        for (int i = 0; i < nchanges; i++) {
            int err = apply(*kq, changes[i]);
            if ((err || (changes[i].flags & EV_RECEIPT)) && n < nevents) {
                DKevent& e = events[n++];
                e = changes[i];
                e.flags = EV_ERROR;
                e.data = err;
            } else if (err && !nevents) {
                set_errno(err);
                return -1;
            }
        }
    }
    if (n || nevents <= 0) return n;
    ULONGLONG deadline = ts ? GetTickCount64() + (ULONGLONG)(ts->tv_sec * 1000 + ts->tv_nsec / 1000000) : ~0ull;
    for (;;) {
        {
            std::lock_guard lk(kq->m);
            n = collect(*kq, events, nevents);
        }
        if (n || GetTickCount64() >= deadline) return n;
        Sleep(1);
    }
}

}  // namespace

HLE_FUNC(kqueue, int, ()) {
    auto f = std::make_shared<fdtab::Fd>();
    f->kind = fdtab::Kind::Kqueue;
    f->kq = std::make_shared<Kq>();
    int fd = fdtab::add(f);
    LOGI("kqueue() = %d", fd);
    return fd;
}

HLE_FUNC(kevent, int, (int kq, const DKevent* changes, int nchanges, DKevent* events, int nevents, const d_timespec* ts)) {
    return do_kevent(kq, changes, nchanges, events, nevents, ts);
}

// kevent64: the same with two extension words per event (unused)
HLE_FUNC(kevent64, int, (int kq, const DKevent64* changes, int nchanges, DKevent64* events, int nevents, u32 flags, const d_timespec* ts)) {
    (void)flags;
    std::vector<DKevent> c(std::max(nchanges, 0)), e(std::max(nevents, 0));
    for (int i = 0; i < nchanges; i++) memcpy(&c[i], &changes[i], sizeof(DKevent));
    int r = do_kevent(kq, c.data(), nchanges, e.data(), nevents, ts);
    for (int i = 0; i < r; i++) {
        memcpy(&events[i], &e[i], sizeof(DKevent));
        events[i].ext[0] = events[i].ext[1] = 0;
    }
    return r;
}
