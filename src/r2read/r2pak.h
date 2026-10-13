// r2read: reads Remnant II's own packages in place for the Remnant x MHW mashup.
// Copyright (C) 2026 Jalon and contributors. GPL-3.0-or-later (links the ooz Oodle-format decoder).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace r2 {

using Bytes = std::vector<uint8_t>;

// One UE5 IoStore container (.utoc + .ucas). Only unencrypted containers are read.
class IoStore {
public:
    bool open(const std::string& utocPath, std::string* err);
    // Paths are matched case-insensitively ("Remnant2/Content/...uasset").
    bool find(const std::string& path, uint32_t* entry) const;
    bool read(uint32_t entry, Bytes* out, std::string* err) const;
    size_t fileCount() const { return index_.size(); }

private:
    struct Block { uint64_t offset; uint32_t csize, usize; uint8_t method; };
    std::string ucas_;
    uint32_t blockSize_ = 0;
    std::vector<uint64_t> offsets_, lengths_;
    std::vector<Block> blocks_;
    std::vector<std::string> methods_;
    struct Entry { uint64_t hash; uint32_t entry; };
    std::vector<Entry> index_;  // sorted by hash of the lower-cased path
    bool readIndex(const uint8_t* b, size_t n);
};

uint64_t pathHash(const std::string& path);

// All containers in a Paks folder.
class Paks {
public:
    bool open(const std::string& paksDir, std::string* err);
    bool read(const std::string& path, Bytes* out, std::string* err) const;
    bool exists(const std::string& path) const;
    size_t fileCount() const;
private:
    std::vector<IoStore> stores_;
};

// Zen package helpers (UE 5.2 cooked, verified against Remnant II).
struct Text { std::string key, source; };
std::vector<Text> texts(const Bytes& pkg);                 // FText key + source string (UTF-8)
std::vector<std::string> names(const Bytes& pkg, size_t* end = nullptr);
struct BulkEntry { int64_t offset, size; uint32_t flags; };
std::vector<BulkEntry> bulkMap(const Bytes& pkg);

struct Mip { int w = 0, h = 0; bool inlineData = false; size_t at = 0; int64_t size = 0, bulkOffset = 0; };
struct Texture { std::string format; int w = 0, h = 0; std::vector<Mip> mips; };
bool parseTexture(const Bytes& pkg, Texture* out, std::string* err);

}  // namespace r2
