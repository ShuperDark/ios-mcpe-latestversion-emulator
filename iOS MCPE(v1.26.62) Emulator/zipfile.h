// A minimal reader for .zip archives (stored and deflated entries), used for shader packs.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

class ZipFile {
public:
    // Reads the archive's directory; false if it is not a zip file.
    bool open(const std::wstring& path);
    // Entry names (as stored, '/' separators), in archive order.
    const std::vector<std::string>& names() const { return names_; }
    bool has(const std::string& name) const { return entries_.count(name) != 0; }
    // The entry's bytes; false if missing or damaged.
    bool read(const std::string& name, std::vector<uint8_t>& out) const;
    bool read_text(const std::string& name, std::string& out) const;

private:
    struct Entry {
        uint32_t local_offset, comp_size, size;
        uint16_t method;
    };
    std::vector<uint8_t> data_;
    std::map<std::string, Entry> entries_;
    std::vector<std::string> names_;
};
