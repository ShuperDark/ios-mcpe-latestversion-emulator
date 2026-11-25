#include "macho.h"
#include "vmem.h"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>

namespace {

#pragma pack(push, 1)
struct mach_header_64 {
    u32 magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved;
};
struct load_command { u32 cmd, cmdsize; };
struct segment_command_64 {
    u32 cmd, cmdsize;
    char segname[16];
    u64 vmaddr, vmsize, fileoff, filesize;
    u32 maxprot, initprot, nsects, flags;
};
struct section_64 {
    char sectname[16], segname[16];
    u64 addr, size;
    u32 offset, align, reloff, nreloc, flags, reserved1, reserved2, reserved3;
};
struct dyld_info_command {
    u32 cmd, cmdsize;
    u32 rebase_off, rebase_size, bind_off, bind_size, weak_bind_off, weak_bind_size;
    u32 lazy_bind_off, lazy_bind_size, export_off, export_size;
};
struct symtab_command { u32 cmd, cmdsize, symoff, nsyms, stroff, strsize; };
struct entry_point_command { u32 cmd, cmdsize; u64 entryoff, stacksize; };
struct dylib_command { u32 cmd, cmdsize, name_off, timestamp, current_version, compat_version; };
struct linkedit_data_command { u32 cmd, cmdsize, dataoff, datasize; };
struct nlist_64 { u32 n_strx; u8 n_type, n_sect; u16 n_desc; u64 n_value; };
struct fat_header { u32 magic, nfat_arch; };
struct fat_arch { u32 cputype, cpusubtype, offset, size, align; };
#pragma pack(pop)

constexpr u32 MH_MAGIC_64 = 0xfeedfacf;
constexpr u32 CPU_TYPE_ARM64 = 0x0100000c;
constexpr u32 LC_SEGMENT_64 = 0x19, LC_SYMTAB = 0x2, LC_DYLD_INFO = 0x22, LC_DYLD_INFO_ONLY = 0x80000022,
              LC_MAIN = 0x80000028, LC_LOAD_DYLIB = 0xc, LC_LOAD_WEAK_DYLIB = 0x80000018,
              LC_REEXPORT_DYLIB = 0x8000001f, LC_LOAD_UPWARD_DYLIB = 0x80000023, LC_LAZY_LOAD_DYLIB = 0x20,
              LC_ID_DYLIB = 0xd, LC_DYLD_CHAINED_FIXUPS = 0x80000034, LC_DYLD_EXPORTS_TRIE = 0x80000033;

u32 bswap32(u32 v) { return _byteswap_ulong(v); }

u64 uleb(const u8*& p, const u8* end) {
    u64 r = 0;
    int sh = 0;
    while (p < end) {
        u8 b = *p++;
        r |= u64(b & 0x7f) << sh;
        if (!(b & 0x80)) break;
        sh += 7;
    }
    return r;
}
s64 sleb(const u8*& p, const u8* end) {
    s64 r = 0;
    int sh = 0;
    u8 b = 0;
    while (p < end) {
        b = *p++;
        r |= s64(b & 0x7f) << sh;
        sh += 7;
        if (!(b & 0x80)) break;
    }
    if (sh < 64 && (b & 0x40)) r |= -(s64(1) << sh);
    return r;
}

std::shared_mutex g_img_lock;
std::vector<std::shared_ptr<MachImage>> g_images;

}  // namespace

const MachSection* MachImage::find_section(const char* seg, const char* sect) const {
    for (auto& s : sections)
        if (s.segname == seg && s.sectname == sect) return &s;
    return nullptr;
}
const MachSegment* MachImage::find_segment(const char* seg) const {
    for (auto& s : segments)
        if (s.name == seg) return &s;
    return nullptr;
}

namespace macho {

static void parse_rebase(MachImage& img, const std::vector<MachSegment>& segs, const u8* p, const u8* end) {
    if (img.slide == 0) return;
    u64 addr = 0;
    int type = 1;
    auto rebase_at = [&](u64 a) {
        u64* ptr = gptr<u64>(a);
        if (type == 1) *ptr += img.slide;
    };
    while (p < end) {
        u8 b = *p++;
        u8 op = b & 0xf0, imm = b & 0x0f;
        switch (op) {
        case 0x00: return;
        case 0x10: type = imm; break;
        case 0x20: addr = segs[imm].addr + uleb(p, end); break;
        case 0x30: addr += uleb(p, end); break;
        case 0x40: addr += u64(imm) * 8; break;
        case 0x50: for (int i = 0; i < imm; i++) { rebase_at(addr); addr += 8; } break;
        case 0x60: { u64 n = uleb(p, end); for (u64 i = 0; i < n; i++) { rebase_at(addr); addr += 8; } } break;
        case 0x70: rebase_at(addr); addr += 8 + uleb(p, end); break;
        case 0x80: {
            u64 n = uleb(p, end), skip = uleb(p, end);
            for (u64 i = 0; i < n; i++) { rebase_at(addr); addr += 8 + skip; }
        } break;
        default: fatal("bad rebase opcode %02x in %s", b, img.path.c_str());
        }
    }
}

static void parse_binds(MachImage& img, const std::vector<MachSegment>& segs, const u8* p, const u8* end, bool lazy, bool weak) {
    u64 addr = 0;
    int ordinal = 0;
    std::string sym;
    u8 symflags = 0;
    s64 addend = 0;
    auto do_bind = [&]() {
        if (weak && (symflags & 0x8)) return;  // strong definition marker
        MachBind b{addr, sym, addend, ordinal, (symflags & 1) != 0, lazy, weak};
        img.binds.push_back(std::move(b));
    };
    while (p < end) {
        u8 b = *p++;
        u8 op = b & 0xf0, imm = b & 0x0f;
        switch (op) {
        case 0x00:
            if (!lazy) return;
            break;  // lazy stream: DONE separates entries
        case 0x10: ordinal = imm; break;
        case 0x20: ordinal = int(uleb(p, end)); break;
        case 0x30: ordinal = imm ? int(s8(0xf0 | imm)) : 0; break;
        case 0x40: {
            symflags = imm;
            const char* s = reinterpret_cast<const char*>(p);
            size_t n = strnlen(s, end - p);
            sym.assign(s, n);
            p += n + 1;
        } break;
        case 0x50: break;  // type (always pointer for arm64)
        case 0x60: addend = sleb(p, end); break;
        case 0x70: addr = segs[imm].addr + uleb(p, end); break;
        case 0x80: addr += uleb(p, end); break;
        case 0x90: do_bind(); addr += 8; break;
        case 0xa0: do_bind(); addr += 8 + uleb(p, end); break;
        case 0xb0: do_bind(); addr += 8 + u64(imm) * 8; break;
        case 0xc0: {
            u64 n = uleb(p, end), skip = uleb(p, end);
            for (u64 i = 0; i < n; i++) { do_bind(); addr += 8 + skip; }
        } break;
        default: fatal("bad bind opcode %02x in %s", b, img.path.c_str());
        }
    }
}

static void parse_trie(MachImage& img, const u8* start, const u8* end, const u8* node, std::string& prefix, int depth) {
    if (node >= end || depth > 128) return;
    const u8* p = node;
    u64 term = uleb(p, end);
    const u8* children = p + term;
    if (term) {
        u64 flags = uleb(p, end);
        if (flags & 0x08) {
            // re-export: ignored (our images never re-export)
        } else {
            u64 off = uleb(p, end);
            if ((flags & 0x3) == 2)
                img.exports[prefix] = off;  // absolute
            else
                img.exports[prefix] = img.base + off;
            img.export_weak[prefix] = (flags & 0x4) != 0;
        }
    }
    p = children;
    if (p >= end) return;
    u8 nch = *p++;
    for (u8 i = 0; i < nch; i++) {
        const char* s = reinterpret_cast<const char*>(p);
        size_t n = strnlen(s, end - p);
        p += n + 1;
        u64 off = uleb(p, end);
        size_t old = prefix.size();
        prefix.append(s, n);
        parse_trie(img, start, end, start + off, prefix, depth + 1);
        prefix.resize(old);
    }
}

// LC_DYLD_CHAINED_FIXUPS (iOS 13.4+ linkers): rebases and binds are encoded in the
// pointers themselves, as linked lists per page. arm64 (not arm64e) uses
// DYLD_CHAINED_PTR_64 (target = vmaddr) or DYLD_CHAINED_PTR_64_OFFSET (target = offset).
static void parse_chained(MachImage& img, const u8* hdr, u32 size) {
    struct Header { u32 version, starts_offset, imports_offset, symbols_offset, imports_count, imports_format, symbols_format; };
    auto* h = reinterpret_cast<const Header*>(hdr);
    if (h->symbols_format != 0) fatal("%s: compressed chained-fixup symbols are not supported", img.path.c_str());
    struct Import { std::string name; int ordinal; bool weak; s64 addend; };
    std::vector<Import> imports(h->imports_count);
    const char* syms = reinterpret_cast<const char*>(hdr + h->symbols_offset);
    for (u32 i = 0; i < h->imports_count; i++) {
        const u8* ip = hdr + h->imports_offset;
        u32 lib = 0, name_off = 0;
        bool weak = false;
        s64 addend = 0;
        if (h->imports_format == 1) {  // DYLD_CHAINED_IMPORT
            u32 v = reinterpret_cast<const u32*>(ip)[i];
            lib = v & 0xff; weak = (v >> 8) & 1; name_off = v >> 9;
            if (lib >= 0xf0) lib |= 0xffffff00u;
        } else if (h->imports_format == 2) {  // DYLD_CHAINED_IMPORT_ADDEND
            u32 v = reinterpret_cast<const u32*>(ip)[i * 2];
            lib = v & 0xff; weak = (v >> 8) & 1; name_off = v >> 9;
            addend = reinterpret_cast<const s32*>(ip)[i * 2 + 1];
            if (lib >= 0xf0) lib |= 0xffffff00u;
        } else if (h->imports_format == 3) {  // DYLD_CHAINED_IMPORT_ADDEND64
            u64 v = reinterpret_cast<const u64*>(ip)[i * 2];
            lib = u32(v & 0xffff); weak = (v >> 16) & 1; name_off = u32(v >> 32);
            addend = reinterpret_cast<const s64*>(ip)[i * 2 + 1];
            if (lib >= 0xfff0) lib |= 0xffff0000u;
        } else fatal("%s: chained import format %u", img.path.c_str(), h->imports_format);
        imports[i] = {syms + name_off, int(s32(lib)), weak, addend};
    }
    const u8* starts = hdr + h->starts_offset;
    u32 seg_count = *reinterpret_cast<const u32*>(starts);
    size_t nrebase = 0;
    for (u32 si = 0; si < seg_count && si < img.segments.size(); si++) {
        u32 off = reinterpret_cast<const u32*>(starts + 4)[si];
        if (!off) continue;
        const u8* ss = starts + off;
        u16 page_size = *reinterpret_cast<const u16*>(ss + 4);
        u16 fmt = *reinterpret_cast<const u16*>(ss + 6);
        u64 seg_off = *reinterpret_cast<const u64*>(ss + 8);
        u16 page_count = *reinterpret_cast<const u16*>(ss + 20);
        const u16* page_start = reinterpret_cast<const u16*>(ss + 22);
        if (fmt != 2 && fmt != 6) fatal("%s: chained pointer format %u is not supported", img.path.c_str(), fmt);
        gaddr seg_addr = img.base + seg_off;  // segment_offset is relative to the mach header
        for (u32 pg = 0; pg < page_count; pg++) {
            u16 st = page_start[pg];
            if (st == 0xffff) continue;
            gaddr a = seg_addr + u64(pg) * page_size + st;
            for (;;) {
                u64 v = *gptr<u64>(a);
                u32 next = u32((v >> 51) & 0xfff);
                if (v >> 63) {  // bind
                    u32 ord = u32(v & 0xffffff);
                    s64 add = s64(s8(u8((v >> 24) & 0xff)));
                    if (ord >= imports.size()) fatal("%s: bad chained import %u", img.path.c_str(), ord);
                    const Import& im = imports[ord];
                    MachBind b{a, im.name, im.addend + add, im.ordinal, im.weak, false, false};
                    *gptr<u64>(a) = 0;
                    img.binds.push_back(std::move(b));
                } else {        // rebase
                    u64 target = v & 0xfffffffffull;
                    u64 high8 = (v >> 36) & 0xff;
                    u64 r = fmt == 6 ? img.base + target : target + img.slide;
                    *gptr<u64>(a) = r | (high8 << 56);
                    nrebase++;
                }
                if (!next) break;
                a += u64(next) * 4;
            }
        }
    }
    LOGD("%s: %zu chained rebases, %u imports", img.install_name.c_str(), nrebase, h->imports_count);
}

std::shared_ptr<MachImage> load(const std::string& host_path, const std::string& guest_path, gaddr preferred_base) {
    std::ifstream f(widen(host_path), std::ios::binary);
    if (!f) fatal("cannot open %s", host_path.c_str());
    std::vector<u8> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const u8* data = file.data();
    size_t fsize = file.size();
    if (fsize >= 8 && *reinterpret_cast<const u32*>(data) == 0xbebafeca) {
        auto* fh = reinterpret_cast<const fat_header*>(data);
        u32 n = bswap32(fh->nfat_arch);
        auto* fa = reinterpret_cast<const fat_arch*>(data + 8);
        bool found = false;
        for (u32 i = 0; i < n; i++) {
            if (bswap32(fa[i].cputype) == CPU_TYPE_ARM64) {
                data += bswap32(fa[i].offset);
                fsize = bswap32(fa[i].size);
                found = true;
                break;
            }
        }
        if (!found) fatal("%s: no arm64 slice", host_path.c_str());
    }
    auto* mh = reinterpret_cast<const mach_header_64*>(data);
    if (mh->magic != MH_MAGIC_64 || mh->cputype != CPU_TYPE_ARM64) fatal("%s: not an arm64 Mach-O", host_path.c_str());

    auto img = std::make_shared<MachImage>();
    img->path = host_path;
    img->install_name = guest_path;
    img->filetype = mh->filetype;
    img->is_main = mh->filetype == 2;

    // ---- pass 1: segments ----
    std::vector<const segment_command_64*> segcmds;
    const dyld_info_command* dyinfo = nullptr;
    const symtab_command* symtab = nullptr;
    const entry_point_command* entry = nullptr;
    const linkedit_data_command* chained = nullptr;
    const linkedit_data_command* exports_trie = nullptr;
    const u8* lc = data + sizeof(mach_header_64);
    for (u32 i = 0; i < mh->ncmds; i++) {
        auto* c = reinterpret_cast<const load_command*>(lc);
        switch (c->cmd) {
        case LC_SEGMENT_64: segcmds.push_back(reinterpret_cast<const segment_command_64*>(c)); break;
        case LC_DYLD_INFO:
        case LC_DYLD_INFO_ONLY: dyinfo = reinterpret_cast<const dyld_info_command*>(c); break;
        case LC_SYMTAB: symtab = reinterpret_cast<const symtab_command*>(c); break;
        case LC_MAIN: entry = reinterpret_cast<const entry_point_command*>(c); break;
        case LC_LOAD_DYLIB:
        case LC_LOAD_WEAK_DYLIB:
        case LC_REEXPORT_DYLIB:
        case LC_LOAD_UPWARD_DYLIB:
        case LC_LAZY_LOAD_DYLIB: {
            auto* d = reinterpret_cast<const dylib_command*>(c);
            img->dylibs.emplace_back(reinterpret_cast<const char*>(c) + d->name_off);
        } break;
        case LC_ID_DYLIB: {
            auto* d = reinterpret_cast<const dylib_command*>(c);
            if (img->install_name.empty()) img->install_name = reinterpret_cast<const char*>(c) + d->name_off;
        } break;
        case LC_DYLD_CHAINED_FIXUPS: chained = reinterpret_cast<const linkedit_data_command*>(c); break;
        case LC_DYLD_EXPORTS_TRIE: exports_trie = reinterpret_cast<const linkedit_data_command*>(c); break;
        case 0x2c: {  // LC_ENCRYPTION_INFO_64 {cmd, cmdsize, cryptoff, cryptsize, cryptid}
            if (reinterpret_cast<const u32*>(c)[4] != 0)
                fatal("%s is encrypted (App Store FairPlay DRM, cryptid != 0). Only a decrypted .ipa can be run.", host_path.c_str());
            break;
        }
        default: break;
        }
        lc += c->cmdsize;
    }

    u64 lo = ~0ull, hi = 0;
    for (auto* s : segcmds) {
        if (s->vmsize == 0) continue;
        if (s->initprot == 0 && s->filesize == 0 && s->vmaddr == 0) continue;  // __PAGEZERO
        lo = std::min(lo, s->vmaddr);
        hi = std::max(hi, s->vmaddr + s->vmsize);
    }
    if (lo == ~0ull) fatal("%s: no segments", host_path.c_str());
    u64 size = align_up(hi - lo, 0x10000);

    gaddr want = preferred_base ? preferred_base : lo;
    gaddr base = 0;
    if (vmem::map_fixed(want, size)) base = want;
    else {
        base = vmem::map_anywhere(size, 0);
        if (!base) fatal("%s: cannot map %llx bytes", host_path.c_str(), (unsigned long long)size);
    }
    img->slide = s64(base) - s64(lo);
    img->vm_start = base;
    img->vm_end = base + size;

    std::vector<MachSegment> segs;  // index == segment index in load commands
    for (auto* s : segcmds) {
        MachSegment ms;
        ms.name.assign(s->segname, strnlen(s->segname, 16));
        ms.addr = s->vmaddr + img->slide;
        ms.vmsize = s->vmsize;
        ms.initprot = s->initprot;
        segs.push_back(ms);
        bool pagezero = s->initprot == 0 && s->filesize == 0 && s->vmaddr == 0;
        if (pagezero) continue;
        if (s->filesize) {
            if (s->fileoff + s->filesize > fsize) fatal("%s: segment out of file", host_path.c_str());
            memcpy(gptr(ms.addr), data + s->fileoff, s->filesize);
        }
        if (s->fileoff == 0 && s->filesize) img->base = ms.addr;
        auto* sec = reinterpret_cast<const section_64*>(s + 1);
        for (u32 k = 0; k < s->nsects; k++) {
            MachSection x;
            x.segname.assign(sec[k].segname, strnlen(sec[k].segname, 16));
            x.sectname.assign(sec[k].sectname, strnlen(sec[k].sectname, 16));
            x.addr = sec[k].addr + img->slide;
            x.size = sec[k].size;
            x.flags = sec[k].flags;
            img->sections.push_back(x);
        }
        if (ms.name == "__TEXT") vmem::add_readonly_range(ms.addr, ms.addr + ms.vmsize);
    }
    img->segments = segs;
    if (entry) img->entry = img->base + entry->entryoff;

    // ---- rebase / binds / exports ----
    if (dyinfo) {
        if (dyinfo->rebase_size)
            parse_rebase(*img, segs, data + dyinfo->rebase_off, data + dyinfo->rebase_off + dyinfo->rebase_size);
        if (dyinfo->bind_size)
            parse_binds(*img, segs, data + dyinfo->bind_off, data + dyinfo->bind_off + dyinfo->bind_size, false, false);
        if (dyinfo->lazy_bind_size)
            parse_binds(*img, segs, data + dyinfo->lazy_bind_off, data + dyinfo->lazy_bind_off + dyinfo->lazy_bind_size, true, false);
        if (dyinfo->weak_bind_size)
            parse_binds(*img, segs, data + dyinfo->weak_bind_off, data + dyinfo->weak_bind_off + dyinfo->weak_bind_size, false, true);
        if (dyinfo->export_size) {
            std::string prefix;
            const u8* t = data + dyinfo->export_off;
            parse_trie(*img, t, t + dyinfo->export_size, t, prefix, 0);
        }
    }
    if (exports_trie && exports_trie->datasize) {
        std::string prefix;
        const u8* t = data + exports_trie->dataoff;
        parse_trie(*img, t, t + exports_trie->datasize, t, prefix, 0);
    }
    if (chained && chained->datasize) parse_chained(*img, data + chained->dataoff, chained->datasize);

    // ---- symbol table (for symbolication) ----
    if (symtab && symtab->nsyms) {
        auto* nl = reinterpret_cast<const nlist_64*>(data + symtab->symoff);
        const char* strs = reinterpret_cast<const char*>(data + symtab->stroff);
        for (u32 i = 0; i < symtab->nsyms; i++) {
            if (nl[i].n_type & 0xe0) continue;          // stab
            if ((nl[i].n_type & 0x0e) != 0x0e) continue;  // N_SECT only
            if (nl[i].n_strx >= symtab->strsize) continue;
            img->symbols[nl[i].n_value + img->slide] = strs + nl[i].n_strx;
            img->definitions.emplace(strs + nl[i].n_strx, nl[i].n_value + img->slide);
        }
    }
    for (auto& [n, a] : img->exports) img->symbols.emplace(a, n);

    // ---- TLV template ----
    gaddr ts = 0, te = 0, tdata_end = 0;
    for (auto& s : img->sections) {
        u32 type = s.flags & 0xff;
        if (type == 0x11 || type == 0x12) {
            if (!ts || s.addr < ts) ts = s.addr;
            te = std::max(te, s.addr + s.size);
            if (type == 0x11) tdata_end = std::max(tdata_end, s.addr + s.size);
        }
    }
    if (ts) {
        img->tlv.template_start = ts;
        img->tlv.template_data_size = tdata_end ? tdata_end - ts : 0;
        img->tlv.template_total_size = te - ts;
    }
    LOGI("loaded %s at %llx-%llx (slide %llx), %zu binds, %zu exports", guest_path.c_str(),
         (unsigned long long)img->vm_start, (unsigned long long)img->vm_end, (unsigned long long)img->slide,
         img->binds.size(), img->exports.size());
    return img;
}

void bind_all(MachImage& img, const SymbolResolver& resolve) {
    for (auto& b : img.binds) {
        if (b.weak_def) {
            // Weak coalescing: keep the image's own definition when it has one.
            if (img.exports.count(b.symbol)) continue;
        }
        // A symbol the image defines itself but does not export (e.g. libc++'s hidden weak
        // tables such as std::__itoa::__digits_base_10, which zig's linker still binds by
        // name) can only mean its own definition.
        if (!img.exports.count(b.symbol)) {
            auto own = img.definitions.find(b.symbol);
            if (own != img.definitions.end()) {
                *gptr<u64>(b.where) = own->second + b.addend;
                continue;
            }
        }
        bool ok = true;
        gaddr v = resolve(img, b, ok);
        if (!ok) continue;
        *gptr<u64>(b.where) = v + b.addend;
    }
}

const std::vector<std::shared_ptr<MachImage>>& images() { return g_images; }

void register_image(std::shared_ptr<MachImage> img) {
    std::unique_lock lk(g_img_lock);
    g_images.push_back(std::move(img));
}

MachImage* image_for_address(gaddr a) {
    std::shared_lock lk(g_img_lock);
    for (auto& i : g_images)
        if (i->contains(a)) return i.get();
    return nullptr;
}

gaddr find_export(const std::string& name) {
    std::shared_lock lk(g_img_lock);
    for (auto& i : g_images) {
        auto it = i->exports.find(name);
        if (it != i->exports.end()) return it->second;
    }
    return 0;
}

std::string symbolicate(gaddr a) {
    MachImage* img = image_for_address(a);
    char buf[64];
    if (!img) {
        snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)a);
        return buf;
    }
    std::string base = img->install_name;
    auto sl = base.rfind('/');
    if (sl != std::string::npos) base = base.substr(sl + 1);
    auto it = img->symbols.upper_bound(a);
    if (it != img->symbols.begin()) {
        --it;
        if (a - it->first < 0x10000) {
            snprintf(buf, sizeof buf, "+0x%llx [0x%llx]", (unsigned long long)(a - it->first),
                     (unsigned long long)(a - img->slide));
            return base + "!" + it->second + buf;
        }
    }
    snprintf(buf, sizeof buf, "!0x%llx", (unsigned long long)(a - img->slide));
    return base + buf;
}

}  // namespace macho
