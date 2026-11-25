#include "zipfile.h"

#include <cstdio>
#include <cstring>

#include "zlib.h"

namespace {
uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }
}  // namespace

bool ZipFile::open(const std::wstring& path) {
    data_.clear();
    entries_.clear();
    names_.clear();
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 22) {
        fclose(f);
        return false;
    }
    data_.resize(size_t(n));
    size_t got = fread(data_.data(), 1, data_.size(), f);
    fclose(f);
    if (got != data_.size()) return false;
    // the end of central directory record (at most 64 KiB of comment after it)
    size_t eocd = std::string::npos;
    for (size_t i = data_.size() - 22;; i--) {
        if (rd32(&data_[i]) == 0x06054b50) {
            eocd = i;
            break;
        }
        if (i == 0 || data_.size() - i > 65557) break;
    }
    if (eocd == std::string::npos) return false;
    uint32_t count = rd16(&data_[eocd + 10]);
    uint32_t cd = rd32(&data_[eocd + 16]);
    size_t p = cd;
    for (uint32_t k = 0; k < count; k++) {
        if (p + 46 > data_.size() || rd32(&data_[p]) != 0x02014b50) return !names_.empty();
        Entry e;
        e.method = rd16(&data_[p + 10]);
        e.comp_size = rd32(&data_[p + 20]);
        e.size = rd32(&data_[p + 24]);
        uint16_t nlen = rd16(&data_[p + 28]), xlen = rd16(&data_[p + 30]), clen = rd16(&data_[p + 32]);
        e.local_offset = rd32(&data_[p + 42]);
        if (p + 46 + nlen > data_.size()) return !names_.empty();
        std::string name(reinterpret_cast<const char*>(&data_[p + 46]), nlen);
        for (char& c : name)
            if (c == '\\') c = '/';
        if (!name.empty() && name.back() != '/') {
            entries_[name] = e;
            names_.push_back(name);
        }
        p += 46 + nlen + xlen + clen;
    }
    return true;
}

bool ZipFile::read(const std::string& name, std::vector<uint8_t>& out) const {
    auto it = entries_.find(name);
    if (it == entries_.end()) return false;
    const Entry& e = it->second;
    size_t lh = e.local_offset;
    if (lh + 30 > data_.size() || rd32(&data_[lh]) != 0x04034b50) return false;
    size_t start = lh + 30 + rd16(&data_[lh + 26]) + rd16(&data_[lh + 28]);
    if (start + e.comp_size > data_.size()) return false;
    out.resize(e.size);
    if (e.method == 0) {
        if (e.comp_size != e.size) return false;
        if (e.size) memcpy(out.data(), &data_[start], e.size);
        return true;
    }
    if (e.method != 8) return false;
    z_stream z{};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return false;
    z.next_in = const_cast<Bytef*>(&data_[start]);
    z.avail_in = e.comp_size;
    z.next_out = out.data();
    z.avail_out = e.size;
    int r = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    return r == Z_STREAM_END && z.total_out == e.size;
}

bool ZipFile::read_text(const std::string& name, std::string& out) const {
    std::vector<uint8_t> b;
    if (!read(name, b)) return false;
    out.assign(b.begin(), b.end());
    return true;
}
