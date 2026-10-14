// r2read: reads Remnant II's own packages in place for the Remnant x MHW mashup.
// Copyright (C) 2026 Jalon and contributors. GPL-3.0-or-later (links the ooz Oodle-format decoder).
#include "r2pak.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#endif

// ooz (GPL-3.0): Kraken/Mermaid/Selkie/Leviathan decoder.
typedef unsigned char byte;
int Kraken_Decompress(const byte* src, size_t src_len, byte* dst, size_t dst_len);

namespace r2 {

namespace {

FILE* openFile(const std::string& path) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
    return _wfopen(w.c_str(), L"rb");
#else
    return fopen(path.c_str(), "rb");
#endif
}

bool seek(FILE* f, uint64_t off) {
#ifdef _WIN32
    return _fseeki64(f, (__int64)off, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)off, SEEK_SET) == 0;
#endif
}

struct FileCloser { void operator()(FILE* f) const { if (f) fclose(f); } };
using FilePtr = std::unique_ptr<FILE, FileCloser>;

bool readAll(const std::string& path, Bytes* out) {
    FilePtr f(openFile(path));
    if (!f) return false;
    fseek(f.get(), 0, SEEK_END);
    long n = ftell(f.get());
    fseek(f.get(), 0, SEEK_SET);
    out->resize((size_t)n);
    return fread(out->data(), 1, (size_t)n, f.get()) == (size_t)n;
}

template <class T> T rd(const uint8_t* p) { T v; memcpy(&v, p, sizeof(T)); return v; }

uint64_t be40(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 5; i++) v = (v << 8) | p[i];
    return v;
}
uint64_t le(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

// FString at b[*pos], returns UTF-8.
bool fstring(const uint8_t* b, size_t n, size_t* pos, std::string* out) {
    if (*pos + 4 > n) return false;
    int32_t len = rd<int32_t>(b + *pos);
    *pos += 4;
    out->clear();
    if (len == 0) return true;
    if (len > 0) {
        if (*pos + (size_t)len > n) return false;
        out->assign((const char*)b + *pos, (size_t)len - 1);
        *pos += (size_t)len;
        return true;
    }
    size_t chars = (size_t)(-len);
    if (*pos + chars * 2 > n) return false;
    for (size_t i = 0; i + 1 < chars; i++) {
        uint32_t c = rd<uint16_t>(b + *pos + i * 2);
        if (c >= 0xD800 && c < 0xDC00 && i + 2 < chars) {
            uint32_t lo = rd<uint16_t>(b + *pos + (i + 1) * 2);
            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
            i++;
        }
        if (c < 0x80) out->push_back((char)c);
        else if (c < 0x800) { out->push_back((char)(0xC0 | (c >> 6))); out->push_back((char)(0x80 | (c & 0x3F))); }
        else if (c < 0x10000) {
            out->push_back((char)(0xE0 | (c >> 12))); out->push_back((char)(0x80 | ((c >> 6) & 0x3F)));
            out->push_back((char)(0x80 | (c & 0x3F)));
        } else {
            out->push_back((char)(0xF0 | (c >> 18))); out->push_back((char)(0x80 | ((c >> 12) & 0x3F)));
            out->push_back((char)(0x80 | ((c >> 6) & 0x3F))); out->push_back((char)(0x80 | (c & 0x3F)));
        }
    }
    *pos += chars * 2;
    return true;
}

}  // namespace

uint64_t pathHash(const std::string& path) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a over the lower-cased path
    for (unsigned char c : path) {
        h ^= (uint64_t)std::tolower(c);
        h *= 1099511628211ull;
    }
    return h;
}

bool IoStore::open(const std::string& utocPath, std::string* err) {
    Bytes d;
    if (!readAll(utocPath, &d) || d.size() < 0x90) { *err = "cannot read " + utocPath; return false; }
    if (memcmp(d.data(), "-==--==--==--==-", 16) != 0) { *err = "not an IoStore toc"; return false; }
    const uint8_t* h = d.data();
    uint32_t hs = rd<uint32_t>(h + 0x14), ec = rd<uint32_t>(h + 0x18), cbc = rd<uint32_t>(h + 0x1C);
    uint32_t cbs = rd<uint32_t>(h + 0x20), cmc = rd<uint32_t>(h + 0x24), cml = rd<uint32_t>(h + 0x28);
    blockSize_ = rd<uint32_t>(h + 0x2C);
    uint32_t dis = rd<uint32_t>(h + 0x30);
    uint8_t flags = h[0x50];
    uint32_t seeds = rd<uint32_t>(h + 0x54), nophc = rd<uint32_t>(h + 0x60);
    if (flags & 2) { *err = "encrypted container (skipped: the mod never decrypts game files)"; return false; }
    size_t p = hs;
    auto need = [&](size_t n) { return p + n <= d.size(); };
    if (!need((size_t)ec * 22)) { *err = "toc truncated"; return false; }
    p += (size_t)ec * 12;  // chunk ids are not needed: files are found through the directory index
    offsets_.resize(ec); lengths_.resize(ec);
    for (uint32_t i = 0; i < ec; i++) {
        offsets_[i] = be40(h + p + i * 10);
        lengths_[i] = be40(h + p + i * 10 + 5);
    }
    p += (size_t)ec * 10 + (size_t)seeds * 4 + (size_t)nophc * 4;
    if (!need((size_t)cbc * cbs)) { *err = "toc truncated (blocks)"; return false; }
    blocks_.resize(cbc);
    for (uint32_t i = 0; i < cbc; i++) {
        const uint8_t* b = h + p + (size_t)i * cbs;
        blocks_[i] = {le(b, 5), (uint32_t)le(b + 5, 3), (uint32_t)le(b + 8, 3), b[11]};
    }
    p += (size_t)cbc * cbs;
    for (uint32_t i = 0; i < cmc; i++) {
        const char* s = (const char*)h + p + (size_t)i * cml;
        methods_.emplace_back(s, strnlen(s, cml));
    }
    p += (size_t)cmc * cml;
    if (flags & 4) {  // signed: hash size, toc + block signatures, per-block hashes
        int32_t hsz = rd<int32_t>(h + p);
        p += 4 + (size_t)hsz * 2 + (size_t)cbc * 20;
    }
    ucas_ = utocPath.substr(0, utocPath.size() - 5) + ".ucas";
    if ((flags & 8) && dis && need(dis)) {
        if (!readIndex(h + p, dis)) { *err = "bad directory index"; return false; }
    }
    return true;
}

bool IoStore::readIndex(const uint8_t* b, size_t n) {
    size_t p = 0;
    std::string mount;
    if (!fstring(b, n, &p, &mount)) return false;
    const std::string kStrip = "../../../";
    if (mount.compare(0, kStrip.size(), kStrip) == 0) mount = mount.substr(kStrip.size());
    struct Dir { uint32_t name, child, next, file; };
    struct File { uint32_t name, next, user; };
    uint32_t nd = rd<uint32_t>(b + p); p += 4;
    std::vector<Dir> dirs(nd);
    memcpy(dirs.data(), b + p, (size_t)nd * 16); p += (size_t)nd * 16;
    uint32_t nf = rd<uint32_t>(b + p); p += 4;
    std::vector<File> files(nf);
    memcpy(files.data(), b + p, (size_t)nf * 12); p += (size_t)nf * 12;
    uint32_t ns = rd<uint32_t>(b + p); p += 4;
    std::vector<std::string> strs(ns);
    for (uint32_t i = 0; i < ns; i++)
        if (!fstring(b, n, &p, &strs[i])) return false;
    const uint32_t NONE = 0xFFFFFFFFu;
    index_.reserve(nf);
    // Iterative walk: (dir index, path prefix).
    std::vector<std::pair<uint32_t, std::string>> stack;
    if (nd) stack.push_back({0, mount});
    while (!stack.empty()) {
        auto [di, prefix] = stack.back();
        stack.pop_back();
        const Dir& d = dirs[di];
        std::string here = prefix + (d.name != NONE ? strs[d.name] + "/" : "");
        for (uint32_t f = d.file; f != NONE; f = files[f].next)
            index_.push_back({pathHash(here + strs[files[f].name]), files[f].user});
        for (uint32_t c = d.child; c != NONE; c = dirs[c].next) stack.push_back({c, here});
    }
    std::sort(index_.begin(), index_.end(), [](const Entry& a, const Entry& b) { return a.hash < b.hash; });
    return true;
}

bool IoStore::find(const std::string& path, uint32_t* entry) const {
    uint64_t h = pathHash(path);
    auto it = std::lower_bound(index_.begin(), index_.end(), h, [](const Entry& e, uint64_t v) { return e.hash < v; });
    if (it == index_.end() || it->hash != h) return false;
    *entry = it->entry;
    return true;
}

bool IoStore::read(uint32_t entry, Bytes* out, std::string* err) const {
    if (entry >= offsets_.size()) { *err = "bad entry"; return false; }
    uint64_t off = offsets_[entry], len = lengths_[entry];
    if (!len) { out->clear(); return true; }
    uint64_t first = off / blockSize_, last = (off + len - 1) / blockSize_;
    FilePtr f(openFile(ucas_));
    if (!f) { *err = "cannot open " + ucas_; return false; }
    Bytes all;
    all.reserve((size_t)((last - first + 1) * blockSize_));
    Bytes raw, dec;
    for (uint64_t bi = first; bi <= last; bi++) {
        if (bi >= blocks_.size()) { *err = "block out of range"; return false; }
        const Block& b = blocks_[bi];
        raw.resize(((size_t)b.csize + 15) & ~(size_t)15);
        if (!seek(f.get(), b.offset) || fread(raw.data(), 1, b.csize, f.get()) != b.csize) {
            *err = "short read"; return false;
        }
        if (b.method == 0) {
            all.insert(all.end(), raw.begin(), raw.begin() + b.usize);
        } else {
            const std::string& m = methods_.at(b.method - 1);
            if (m != "Oodle") { *err = "unsupported compression " + m; return false; }
            dec.assign((size_t)b.usize + 64, 0);
            int got = Kraken_Decompress(raw.data(), b.csize, dec.data(), b.usize);
            if (got != (int)b.usize) { *err = "oodle block decode failed"; return false; }
            all.insert(all.end(), dec.begin(), dec.begin() + b.usize);
        }
    }
    size_t start = (size_t)(off - first * blockSize_);
    out->assign(all.begin() + start, all.begin() + start + (size_t)len);
    return true;
}

bool Paks::open(const std::string& dir, std::string* err) {
    for (const char* n : {"pakchunk0-Windows.utoc", "pakchunk1-Windows.utoc"}) {
        IoStore s;
        std::string e;
        if (s.open(dir + "/" + n, &e)) stores_.push_back(std::move(s));
        else if (err->empty()) *err = e;
    }
    // Any further pakchunkN containers (DLC) are picked up too.
    for (int i = 2; i < 64; i++) {
        IoStore s;
        std::string e;
        if (s.open(dir + "/pakchunk" + std::to_string(i) + "-Windows.utoc", &e)) stores_.push_back(std::move(s));
    }
    if (stores_.empty()) return false;
    err->clear();
    return true;
}

bool Paks::exists(const std::string& path) const {
    uint32_t e;
    for (const auto& s : stores_)
        if (s.find(path, &e)) return true;
    return false;
}

bool Paks::read(const std::string& path, Bytes* out, std::string* err) const {
    uint32_t e;
    for (const auto& s : stores_)
        if (s.find(path, &e)) return s.read(e, out, err);
    *err = "not found: " + path;
    return false;
}

size_t Paks::fileCount() const {
    size_t n = 0;
    for (const auto& s : stores_) n += s.fileCount();
    return n;
}

// ---------------------------------------------------------------- zen package helpers

namespace {
bool isHexKey(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
    return true;
}

// Every plausible serialized FString, in order (same rules as tools/r2text.py).
std::vector<std::string> allStrings(const Bytes& d) {
    std::vector<std::string> out;
    size_t i = 0, n = d.size();
    while (i + 4 < n) {
        int32_t len = rd<int32_t>(d.data() + i);
        if (len >= 2 && len <= 4096 && i + 4 + (size_t)len <= n && d[i + 4 + len - 1] == 0) {
            bool ok = true;
            for (int k = 0; k < len - 1; k++) {
                uint8_t c = d[i + 4 + k];
                if (!((c >= 32 && c < 127) || c == 9 || c == 10 || c == 13)) { ok = false; break; }
            }
            if (ok) {
                out.emplace_back((const char*)d.data() + i + 4, (size_t)len - 1);
                i += 4 + (size_t)len;
                continue;
            }
        }
        if (len <= -2 && len >= -4096 && i + 4 + (size_t)(-len) * 2 <= n) {
            size_t chars = (size_t)(-len);
            if (rd<uint16_t>(d.data() + i + 4 + (chars - 1) * 2) == 0) {
                bool ok = true;
                for (size_t k = 0; k + 1 < chars; k++) {
                    uint16_t c = rd<uint16_t>(d.data() + i + 4 + k * 2);
                    if (c < 32 && c != 9 && c != 10 && c != 13) { ok = false; break; }
                    if (c == 0xFFFF || c == 0xFFFE) { ok = false; break; }
                }
                if (ok) {
                    size_t pos = i;
                    std::string s;
                    fstring(d.data(), n, &pos, &s);
                    out.push_back(s);
                    i = pos;
                    continue;
                }
            }
        }
        i++;
    }
    return out;
}
}  // namespace

std::vector<Text> texts(const Bytes& pkg) {
    std::vector<Text> out;
    std::vector<std::string> s = allStrings(pkg);
    for (size_t i = 0; i + 1 < s.size(); i++)
        if (isHexKey(s[i]) && !isHexKey(s[i + 1])) out.push_back({s[i], s[i + 1]});
    return out;
}

std::vector<std::string> names(const Bytes& d, size_t* end) {
    std::vector<std::string> res;
    if (d.size() < 16) return res;
    uint32_t hdr = rd<uint32_t>(d.data() + 4);
    for (size_t off = 16; off < std::min<size_t>(hdr, 0x200); off += 4) {
        if (off + 16 > d.size()) break;
        uint32_t cnt = rd<uint32_t>(d.data() + off), nbytes = rd<uint32_t>(d.data() + off + 4);
        if (!(cnt > 0 && cnt < 100000 && nbytes > 0 && nbytes < 10000000)) continue;
        if (off + 16 + (size_t)cnt * 10 + nbytes > d.size()) continue;
        if (rd<uint64_t>(d.data() + off + 8) != 0xC1640000ull) continue;
        size_t p = off + 16 + (size_t)cnt * 8;
        std::vector<uint16_t> lens(cnt);
        for (uint32_t i = 0; i < cnt; i++) lens[i] = (uint16_t)((d[p + i * 2] << 8) | d[p + i * 2 + 1]);
        p += (size_t)cnt * 2;
        for (uint32_t i = 0; i < cnt; i++) {
            uint16_t L = lens[i];
            if (L & 0x8000) {
                L &= 0x7FFF;
                p += p & 1;
                std::string s;
                for (uint16_t k = 0; k < L; k++) {
                    uint16_t c = rd<uint16_t>(d.data() + p + k * 2);
                    s.push_back(c < 0x80 ? (char)c : '?');
                }
                res.push_back(s);
                p += (size_t)L * 2;
            } else {
                res.emplace_back((const char*)d.data() + p, L);
                p += L;
            }
        }
        if (end) *end = p;
        return res;
    }
    return res;
}

std::vector<BulkEntry> bulkMap(const Bytes& d) {
    std::vector<BulkEntry> out;
    size_t p = 0;
    names(d, &p);
    if (!p || p + 8 > d.size()) return out;
    uint64_t size = rd<uint64_t>(d.data() + p);
    p += 8;
    for (uint64_t i = 0; i < size / 32 && p + (i + 1) * 32 <= d.size(); i++) {
        const uint8_t* e = d.data() + p + i * 32;
        out.push_back({rd<int64_t>(e), rd<int64_t>(e + 16), rd<uint32_t>(e + 24)});
    }
    return out;
}

// Virtual textures (UE 5.2 FVirtualTextureBuiltData): the platform data holds no classic mips (NumMips == 0); after
// bIsVirtual come the tile layout and data chunks, each chunk an entry of the bulk-data map.
static bool parseVirtual(const Bytes& d, size_t p, const std::string& pf, const std::vector<BulkEntry>& bmap,
                         Texture* out, std::string* err) {
    auto i32 = [&](int32_t* v) {
        if (p + 4 > d.size()) return false;
        *v = rd<int32_t>(d.data() + p);
        p += 4;
        return true;
    };
    auto arr = [&](std::vector<int32_t>* v) {
        int32_t n;
        if (!i32(&n) || n < 0 || n > 4096 || p + (size_t)n * 4 > d.size()) return false;
        v->resize((size_t)n);
        for (int32_t i = 0; i < n; i++) i32(&(*v)[(size_t)i]);
        return true;
    };
    int32_t virt, cooked, layers, wBlocks, hBlocks, tile, border, nm, w, h;
    std::vector<int32_t> layerOff, chunkOfMip, baseOfMip;
    if (!i32(&virt) || virt != 1 || !i32(&cooked) || !i32(&layers) || !i32(&wBlocks) || !i32(&hBlocks) ||
        !i32(&tile) || !i32(&border) || !arr(&layerOff) || !i32(&nm) || !i32(&w) || !i32(&h) ||
        !arr(&chunkOfMip) || !arr(&baseOfMip)) { *err = "bad virtual texture"; return false; }
    if (layers != 1 || wBlocks != 1 || hBlocks != 1 || layerOff.size() != 1 || nm <= 0 || nm > 16 ||
        chunkOfMip.size() != (size_t)nm || baseOfMip.size() != (size_t)nm || tile <= 0 || tile % 4 || border % 4) {
        *err = "unsupported virtual texture";
        return false;
    }
    struct Off { int32_t w, h, maxAddr; std::vector<int32_t> addr, off; };
    int32_t nOff;
    if (!i32(&nOff) || nOff != nm) { *err = "bad virtual tile offsets"; return false; }
    std::vector<Off> offs((size_t)nm);
    for (Off& o : offs)
        if (!i32(&o.w) || !i32(&o.h) || !i32(&o.maxAddr) || !arr(&o.addr) || !arr(&o.off) || o.addr.size() != o.off.size() ||
            o.w <= 0 || o.h <= 0 || o.w > 1024 || o.h > 1024) { *err = "bad virtual tile offsets"; return false; }
    // Three zero ints (empty trailing tables) sit between the tile offsets and the layer format string.
    for (int z = 0; z < 8 && p + 4 <= d.size() && rd<int32_t>(d.data() + p) == 0; z++) p += 4;
    int32_t slen;  // layer pixel format (FString), then the layer fallback colour (4 floats)
    if (!i32(&slen) || slen < 2 || slen > 32 || p + (size_t)slen + 16 > d.size()) { *err = "bad virtual layer"; return false; }
    p += (size_t)slen + 16;
    int32_t nChunks;
    if (!i32(&nChunks) || nChunks < 0 || nChunks > 64) { *err = "bad virtual chunks"; return false; }
    std::vector<int32_t> chunkBulk;
    for (int32_t c = 0; c < nChunks; c++) {
        // FIoHash(20) + SizeInBytes + CodecPayloadSize + codec type + payload offset + bulk index
        if (p + 20 + 8 + 5 + 4 > d.size()) { *err = "truncated virtual chunk"; return false; }
        if (d[p + 28] != 4) { *err = "unsupported virtual codec"; return false; }  // 4 = RawGPU
        chunkBulk.push_back(rd<int32_t>(d.data() + p + 33));
        p += 37;
    }
    if (pf == "PF_B8G8R8A8") { *err = "unsupported virtual format"; return false; }
    int blockBytes = (pf == "PF_DXT1" || pf == "PF_BC4") ? 8 : 16;
    int tb = (tile + 2 * border) / 4;
    for (int m = 0; m < nm; m++) {
        int32_t ci = chunkOfMip[(size_t)m];
        if (ci < 0 || (size_t)ci >= chunkBulk.size() || chunkBulk[(size_t)ci] < 0 || (size_t)chunkBulk[(size_t)ci] >= bmap.size()) {
            *err = "bad virtual chunk";
            return false;
        }
        const Off& o = offs[(size_t)m];
        Mip mp;
        mp.vt = true;
        mp.w = std::max(1, w >> m);
        mp.h = std::max(1, h >> m);
        mp.tilesW = o.w; mp.tilesH = o.h;
        mp.tileBlocks = tb; mp.borderBlocks = border / 4; mp.blockBytes = blockBytes;
        mp.tileBytes = layerOff[0];
        mp.bulkOffset = bmap[(size_t)chunkBulk[(size_t)ci]].offset + baseOfMip[(size_t)m];
        mp.tiles.assign((size_t)o.w * o.h, UINT32_MAX);
        for (int y = 0; y < o.h; y++)
            for (int x = 0; x < o.w; x++) {
                uint32_t addr = 0;  // z-order (Morton) address of the tile
                for (int b = 0; b < 16; b++) addr |= ((uint32_t)(x >> b & 1) << (2 * b)) | ((uint32_t)(y >> b & 1) << (2 * b + 1));
                int best = -1;
                for (size_t k = 0; k < o.addr.size(); k++)
                    if ((uint32_t)o.addr[k] <= addr) best = (int)k;
                if (best < 0 || (uint32_t)o.off[(size_t)best] == UINT32_MAX) continue;
                mp.tiles[(size_t)y * o.w + x] = (uint32_t)o.off[(size_t)best] + (addr - (uint32_t)o.addr[(size_t)best]);
            }
        mp.size = (int64_t)((mp.w + 3) / 4) * ((mp.h + 3) / 4) * blockBytes;
        out->mips.push_back(mp);
    }
    return true;
}

bool vtAssemble(const Mip& m, const Bytes& bulk, Bytes* out, std::string* err) {
    int bw = (m.w + 3) / 4, bh = (m.h + 3) / 4, inner = m.tileBlocks - 2 * m.borderBlocks;
    if (inner <= 0) { *err = "bad virtual tile"; return false; }
    out->assign((size_t)m.size, 0);
    for (int ty = 0; ty < m.tilesH; ty++)
        for (int tx = 0; tx < m.tilesW; tx++) {
            uint32_t ti = m.tiles[(size_t)ty * m.tilesW + tx];
            if (ti == UINT32_MAX) continue;
            int64_t base = m.bulkOffset + (int64_t)ti * m.tileBytes;
            if (base < 0 || (size_t)(base + m.tileBytes) > bulk.size()) { *err = "bulk range"; return false; }
            for (int by = 0; by < inner; by++) {
                int dy = ty * inner + by;
                if (dy >= bh) break;
                for (int bx = 0; bx < inner; bx++) {
                    int dx = tx * inner + bx;
                    if (dx >= bw) break;
                    const uint8_t* src = bulk.data() + base + ((int64_t)(by + m.borderBlocks) * m.tileBlocks + bx + m.borderBlocks) * m.blockBytes;
                    std::copy(src, src + m.blockBytes, out->begin() + ((size_t)dy * bw + dx) * m.blockBytes);
                }
            }
        }
    return true;
}

bool parseTexture(const Bytes& d, Texture* out, std::string* err) {
    static const char* kFormats[] = {"PF_BC7", "PF_DXT5", "PF_DXT1", "PF_BC5", "PF_BC4", "PF_B8G8R8A8"};
    size_t off = 0;
    std::string pf;
    for (size_t i = 12; i + 12 < d.size() && pf.empty(); i++) {
        int32_t len = rd<int32_t>(d.data() + i);
        if (len < 7 || len > 12 || i + 4 + (size_t)len > d.size()) continue;
        std::string s((const char*)d.data() + i + 4, (size_t)len - 1);
        for (const char* f : kFormats)
            if (s == f && d[i + 4 + len - 1] == 0) { pf = s; off = i; break; }
    }
    if (pf.empty()) { *err = "no pixel format"; return false; }
    std::vector<BulkEntry> bmap = bulkMap(d);
    out->format = pf;
    out->w = rd<int32_t>(d.data() + off - 12);
    out->h = rd<int32_t>(d.data() + off - 8);
    uint32_t packed = rd<uint32_t>(d.data() + off - 4);
    size_t p = off + 4 + pf.size() + 1;
    if (packed & (1u << 30)) p += 8;
    p += 4;  // first mip to serialize
    int32_t nmips = rd<int32_t>(d.data() + p);
    p += 4;
    if (nmips == 0) return parseVirtual(d, p, pf, bmap, out, err);
    if (nmips < 0 || nmips > 16) { *err = "bad mip count"; return false; }
    const uint32_t kSeparate = 0x0100 | 0x0800 | 0x0001;
    for (int i = 0; i < nmips; i++) {
        if (p + 4 > d.size()) { *err = "truncated mips"; return false; }
        int32_t bi = rd<int32_t>(d.data() + p);
        p += 4;
        if (bi < 0 || (size_t)bi >= bmap.size()) { *err = "bad bulk index"; return false; }
        Mip m;
        m.size = bmap[bi].size;
        m.bulkOffset = bmap[bi].offset;
        if (!(bmap[bi].flags & kSeparate)) {
            m.inlineData = true;
            m.at = p;
            p += (size_t)m.size;
        }
        if (p + 12 > d.size()) { *err = "truncated mip"; return false; }
        m.w = rd<int32_t>(d.data() + p);
        m.h = rd<int32_t>(d.data() + p + 4);
        p += 12;
        out->mips.push_back(m);
    }
    return true;
}

}  // namespace r2
