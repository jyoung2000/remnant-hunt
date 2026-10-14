// mhwread: reads files out of Monster Hunter: World chunk archives in place (nothing is copied unless GET is asked).
//   mhwread <game dir> LIST [filter]          matching paths, sizes, source chunk (higher priority listed last)
//   mhwread <game dir> GET <path> <out file>  one file's bytes, highest-priority chunk wins
//   mhwread <game dir> BONES <path.mod3> [out.json]  skeleton of a MOD3 model (layout in tools/mhwchunk_notes.md)
// Format (see tools/mhwchunk_notes.md): chunkX.bin = u32 magic, u32 count, count*8-byte entries (compressed size, offset);
// each entry is one 0x40000-byte block of a virtual "PKG": Kraken/Oodle-compressed (or stored when size 0), then XORed
// with a 16-byte key chosen per block from a key sequence. The PKG holds a parent/child file table at 0x100.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#include <dirent.h>

typedef uint8_t byte;
int Kraken_Decompress(const byte* src, size_t src_len, byte* dst, size_t dst_len);

static const size_t kBlock = 0x40000;
static const byte kKeys[16][16] = {
 {0xac,0x76,0xcb,0x97,0xec,0x75,0x00,0x13,0x3a,0x81,0x03,0x8e,0x7a,0x82,0xc8,0x0a},
 {0x6b,0xb5,0xc9,0x56,0xe4,0x4d,0x00,0xbc,0x30,0x52,0x33,0xcf,0xbf,0xaa,0xfa,0x25},
 {0x8f,0x02,0x1d,0xcc,0xb0,0xf2,0x78,0x72,0x06,0xfb,0xde,0xe2,0x39,0x0b,0xbb,0x5c},
 {0xda,0x5c,0x1e,0x53,0x1d,0x83,0x59,0x15,0x78,0x75,0xbd,0x63,0xbf,0xaa,0xfa,0x25},
 {0x1f,0x6d,0x31,0xc8,0x83,0xdd,0x71,0x6d,0x7e,0x8f,0x59,0x8c,0xe2,0x3f,0x19,0x29},
 {0x4b,0xb0,0xde,0x04,0xe4,0xe0,0x85,0x69,0x80,0xcc,0xb2,0x94,0x2f,0x9c,0xe9,0xf9},
 {0x8b,0xce,0x54,0xdc,0x4c,0x11,0x13,0x9a,0x78,0x75,0xbd,0x63,0xbf,0xaa,0xfa,0x25},
 {0xec,0x13,0x34,0x59,0x66,0xce,0x73,0x12,0x44,0x00,0x89,0xa2,0xce,0xdd,0xce,0xe9},
 {0xe4,0x66,0x2c,0x70,0x9c,0x75,0x3a,0x03,0x9a,0x2c,0x0f,0x5a,0xe2,0x3f,0x19,0x29},
 {0xa4,0x92,0xfc,0x90,0x33,0x94,0x9c,0x15,0xa0,0x33,0xac,0x22,0x37,0x35,0xcc,0xa7},
 {0x25,0x09,0x9c,0x1f,0x91,0x1a,0x26,0xe5,0xce,0x91,0x72,0xf0,0x7a,0x82,0xc8,0x0a},
 {0xd1,0xd2,0x9d,0x74,0x46,0xd4,0xfd,0xf1,0xa0,0x33,0xac,0x22,0x37,0x35,0xcc,0xa7},
 {0x7e,0xb2,0x68,0x37,0x3b,0x5d,0x36,0x1e,0xd6,0xd3,0x13,0xe2,0x93,0x3c,0x4d,0xcb},
 {0xa1,0xc7,0xd2,0xea,0x66,0x18,0x95,0xac,0x78,0x75,0xbd,0x63,0xbf,0xaa,0xfa,0x25},
 {0x82,0xa4,0x3b,0x21,0x08,0x79,0x7c,0x6a,0x44,0x00,0x89,0xa2,0xce,0xdd,0xce,0xe9},
 {0x41,0xd0,0x55,0xb3,0xdd,0x60,0x15,0x16,0x7e,0x8f,0x59,0x8c,0xe2,0x3f,0x19,0x29}};

static std::vector<byte> gKeySeq;   // WorldChunkTool's keyseq.key: byte [8+i] = key index of block i

struct Blk { uint64_t off; uint32_t csize; };
struct Ent { std::string path; uint64_t size, off; int type; };

struct Pkg {
    std::string name; int prio = 0;
    std::ifstream f;
    std::vector<Blk> blk;
    std::vector<Ent> ents;
    std::map<size_t, std::vector<byte>> cache;   // tiny block cache
    bool open(const std::string& p) {
        f.open(p, std::ios::binary);
        if (!f) return false;
        byte h[8]; f.read((char*)h, 8);
        uint32_t n; memcpy(&n, h + 4, 4);
        std::vector<byte> t((size_t)n * 8); f.read((char*)t.data(), t.size());
        if (!f) return false;
        for (uint32_t i = 0; i < n; i++) {
            const byte* e = &t[(size_t)i * 8];
            // 3 size bytes: size = (b0>>4) + (b1<<4) + (b2<<12)  (WorldChunkTool's nibble shuffle); 5 offset bytes LE
            uint32_t cs = (e[0] >> 4) + ((uint32_t)e[1] << 4) + ((uint32_t)e[2] << 12);
            uint64_t off = 0; for (int k = 0; k < 5; k++) off |= (uint64_t)e[3 + k] << (8 * k);
            blk.push_back({off, cs});
        }
        return true;
    }
    const std::vector<byte>& block(size_t i) {
        auto it = cache.find(i); if (it != cache.end()) return it->second;
        if (cache.size() > 64) cache.clear();
        std::vector<byte>& out = cache[i];
        out.assign(kBlock + 64, 0);
        const Blk& b = blk[i];
        f.clear(); f.seekg((std::streamoff)b.off);
        if (b.csize == 0) { f.read((char*)out.data(), kBlock); out.resize(kBlock); }
        else {
            std::vector<byte> raw(((size_t)b.csize + 15 & ~(size_t)15) + 64); f.read((char*)raw.data(), b.csize);
            int got = Kraken_Decompress(raw.data(), b.csize, out.data(), kBlock);
            if (got <= 0) { fprintf(stderr, "decompress failed %s block %zu\n", name.c_str(), i); out.clear(); return out; }
            out.resize(got);
            if ((size_t)got != kBlock) fprintf(stderr, "%s block %zu: %d bytes\n", name.c_str(), i, got);
        }
        if (i + 8 < gKeySeq.size()) {
            const byte* k = kKeys[gKeySeq[i + 8] & 15];
            for (size_t j = 0; j < out.size(); j++) out[j] ^= k[j & 15];
        }
        return out;
    }
    bool read(uint64_t off, uint64_t len, std::vector<byte>& dst) {
        dst.clear(); dst.reserve(len);
        while (len) {
            size_t bi = off / kBlock; if (bi >= blk.size()) return false;
            const std::vector<byte>& b = block(bi);
            size_t in = off % kBlock; if (in >= b.size()) return false;
            size_t n = (size_t)std::min<uint64_t>(len, b.size() - in);
            dst.insert(dst.end(), b.begin() + in, b.begin() + in + n);
            off += n; len -= n;
        }
        return true;
    }
    template <class T> static T rd(const byte* p) { T v; memcpy(&v, p, sizeof v); return v; }
    bool parse() {
        std::vector<byte> h; if (!read(0, 0x100, h)) return false;
        uint32_t np = rd<uint32_t>(&h[0x0C]), nc = rd<uint32_t>(&h[0x10]);
        if (np > 100000 || nc > 5000000) { fprintf(stderr, "%s: bad table counts (%u,%u): key/layout mismatch?\n", name.c_str(), np, nc); return false; }
        std::vector<byte> t; if (!read(0x100, (uint64_t)np * 0x54 + (uint64_t)nc * 0xB8, t)) return false;
        size_t p = 0;
        for (uint32_t i = 0; i < np; i++) {
            uint32_t cc = rd<uint32_t>(&t[p + 0x50]); p += 0x54;
            for (uint32_t j = 0; j < cc; j++) {
                if (p + 0xB8 > t.size()) return false;
                const byte* e = &t[p];
                Ent en; en.size = rd<uint64_t>(e + 0xA0); en.off = rd<uint64_t>(e + 0xA8); en.type = rd<int32_t>(e + 0xB0);
                size_t nl = en.type == 2 ? 0x50 : 0xA0;
                en.path.assign((const char*)e, strnlen((const char*)e, nl));
                std::replace(en.path.begin(), en.path.end(), '\\', '/');
                p += 0xB8;
                if (en.type == 0 || en.type == 2) ents.push_back(en);
            }
        }
        return true;
    }
};

static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

static void scanDir(const std::string& dir, int group, std::vector<Pkg*>& out) {
    DIR* d = opendir(dir.c_str()); if (!d) return;
    std::vector<std::pair<int, std::string>> fl;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 9 && n.compare(0, 5, "chunk") == 0 && n.substr(n.size() - 4) == ".bin") {
            size_t i = 5; while (i < n.size() && !isdigit((unsigned char)n[i])) i++;
            fl.push_back({atoi(n.c_str() + i), n});
        }
    }
    closedir(d);
    std::sort(fl.begin(), fl.end());
    for (size_t k = 0; k < fl.size(); k++) {
        Pkg* p = new Pkg; p->name = (group == 2 ? "chunkHiPC/" : "chunk/") + fl[k].second; p->prio = group * 1000 + fl[k].first;
        if (!p->open(dir + "/" + fl[k].second) || !p->parse()) { fprintf(stderr, "skip %s\n", p->name.c_str()); delete p; continue; }
        out.push_back(p);
    }
}

// MOD3 skeleton. Header: "MOD\0", u16 version @4, u16 boneCount @6, u64 boneOffset @0x30.
// Bone section: n*24-byte entries {u16 functionId, u8 parent (0xFF root), u8 self index, f32 unk, f32 length, f32 x,y,z (local)},
// n*64 local 4x4 (row-vector, translation in row 3), n*64 inverse-bind 4x4, then a 512-byte u8 table functionId -> bone index (0xFF none).
struct Bone { int idx, fn, parent; float unk, len, lp[3], L[16], IB[16], W[16]; };
static int bonesCmd(const std::vector<byte>& d, const char* outPath) {
    if (d.size() < 0x140 || memcmp(d.data(), "MOD", 4) != 0) { fprintf(stderr, "not a MOD3\n"); return 1; }
    int n = Pkg::rd<uint16_t>(&d[6]); uint64_t bo = Pkg::rd<uint64_t>(&d[0x30]);
    if (n <= 0 || bo + (uint64_t)n * 152 + 512 > d.size()) { fprintf(stderr, "bad bone table (n=%d off=%llu)\n", n, (unsigned long long)bo); return 1; }
    std::vector<Bone> b(n);
    int bad = 0;
    for (int i = 0; i < n; i++) {
        const byte* e = &d[bo + (size_t)i * 24]; Bone& k = b[i];
        k.idx = i; k.fn = Pkg::rd<uint16_t>(e); k.parent = e[2] == 0xFF ? -1 : e[2];
        if (k.parent >= n) bad++;
        k.unk = Pkg::rd<float>(e + 4); k.len = Pkg::rd<float>(e + 8);
        for (int j = 0; j < 3; j++) k.lp[j] = Pkg::rd<float>(e + 12 + 4 * j);
        memcpy(k.L, &d[bo + (size_t)n * 24 + (size_t)i * 64], 64);
        memcpy(k.IB, &d[bo + (size_t)n * 88 + (size_t)i * 64], 64);
    }
    double maxPosDiff = 0, maxIdErr = 0, maxRot = 0;
    for (int i = 0; i < n; i++) {   // parents may come later in the table, so resolve recursively
        std::vector<int> chain; for (int c = i; c >= 0; c = b[c].parent) { chain.push_back(c); if (chain.size() > (size_t)n) { fprintf(stderr, "parent cycle\n"); return 1; } }
        float W[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        for (size_t c = 0; c < chain.size(); c++) {   // W = L_i * L_parent * ... (row vectors)
            float R[16]; const float* A = W; const float* B = b[chain[c]].L;
            for (int r = 0; r < 4; r++) for (int q = 0; q < 4; q++) { R[r*4+q] = 0; for (int t = 0; t < 4; t++) R[r*4+q] += A[r*4+t] * B[t*4+q]; }
            memcpy(W, R, 64);
        }
        memcpy(b[i].W, W, 64);
        Bone& k = b[i];
        for (int j = 0; j < 3; j++) maxPosDiff = std::max(maxPosDiff, (double)fabsf(k.L[12 + j] - k.lp[j]));
        for (int r = 0; r < 3; r++) { double l = 0; for (int q = 0; q < 3; q++) l += (double)k.L[r*4+q] * k.L[r*4+q]; maxRot = std::max(maxRot, fabs(sqrt(l) - 1)); }
        // inverse bind * world should be identity
        for (int r = 0; r < 4; r++) for (int q = 0; q < 4; q++) { double v = 0; for (int t = 0; t < 4; t++) v += (double)k.W[r*4+t] * k.IB[t*4+q]; maxIdErr = std::max(maxIdErr, fabs(v - (r == q))); }
    }
    printf("bones=%d boneOffset=0x%llx structural-bad=%d  |L.t-entry.pos|max=%.3g  rotRowLen-1 max=%.3g  |W*IB-I| max=%.3g\n", n, (unsigned long long)bo, bad, maxPosDiff, maxRot, maxIdErr);
    // depth, chain lengths, height
    int maxDepth = 0; double lo[3] = {1e30,1e30,1e30}, hi[3] = {-1e30,-1e30,-1e30};
    std::vector<int> depth(n), kids(n, 0);
    for (int i = 0; i < n; i++) { int dd = 0; for (int c = b[i].parent; c >= 0; c = b[c].parent) dd++; depth[i] = dd; maxDepth = std::max(maxDepth, dd); if (b[i].parent >= 0) kids[b[i].parent]++;
        for (int j = 0; j < 3; j++) { lo[j] = std::min(lo[j], (double)b[i].W[12+j]); hi[j] = std::max(hi[j], (double)b[i].W[12+j]); } }
    printf("max depth=%d  world bbox min=(%.1f %.1f %.1f) max=(%.1f %.1f %.1f)  height(y)=%.1f units\n", maxDepth, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], hi[1] - lo[1]);
    for (int i = 0; i < n; i++) if (!kids[i] && depth[i] >= 3) {   // leaf chains
        double len = 0; std::string s; std::vector<int> ch; for (int c = i; c >= 0; c = b[c].parent) ch.push_back(c);
        for (size_t k = 0; k + 1 < ch.size(); k++) { const float* a = b[ch[k]].W; const float* p = b[ch[k+1]].W; len += sqrt((a[12]-p[12])*(a[12]-p[12]) + (a[13]-p[13])*(a[13]-p[13]) + (a[14]-p[14])*(a[14]-p[14])); }
        printf("leaf %d (fn %d) depth %d chain length %.1f\n", i, b[i].fn, depth[i], len);
    }
    if (outPath) {
        FILE* o = fopen(outPath, "w"); if (!o) { fprintf(stderr, "cannot write %s\n", outPath); return 1; }
        fprintf(o, "[\n");
        for (int i = 0; i < n; i++) { const Bone& k = b[i];
            fprintf(o, " {\"index\":%d,\"id\":%d,\"parent\":%d,\"length\":%.6g,\"local_pos\":[%.6g,%.6g,%.6g],\"local_rot_matrix\":[[%.7g,%.7g,%.7g],[%.7g,%.7g,%.7g],[%.7g,%.7g,%.7g]],\"world_pos\":[%.6g,%.6g,%.6g]}%s\n",
                k.idx, k.fn, k.parent, k.len, k.L[12], k.L[13], k.L[14], k.L[0], k.L[1], k.L[2], k.L[4], k.L[5], k.L[6], k.L[8], k.L[9], k.L[10], k.W[12], k.W[13], k.W[14], i + 1 < n ? "," : ""); }
        fprintf(o, "]\n"); fclose(o); printf("wrote %s\n", outPath);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: mhwread <game dir> LIST [filter] | GET <path> <out>\n"); return 2; }
    std::string game = argv[1], cmd = argv[2];
    const char* ks = getenv("MHW_KEYSEQ");
    { std::ifstream k(ks ? ks : "build/mhw/keyseq.key", std::ios::binary);
      gKeySeq.assign(std::istreambuf_iterator<char>(k), std::istreambuf_iterator<char>());
      if (gKeySeq.size() < 1000) { fprintf(stderr, "keyseq.key not found (set MHW_KEYSEQ)\n"); return 1; } }
    std::vector<Pkg*> pk;
    scanDir(game + "/chunk", 1, pk);
    scanDir(game + "/chunkHiPC", 2, pk);
    std::stable_sort(pk.begin(), pk.end(), [](Pkg* a, Pkg* b) { return a->prio < b->prio; });
    if (cmd == "LIST") {
        std::string flt = argc > 3 ? lower(argv[3]) : "";
        for (Pkg* p : pk) for (auto& e : p->ents)
            if (flt.empty() || lower(e.path).find(flt) != std::string::npos) printf("%10llu  %-18s  %s\n", (unsigned long long)e.size, p->name.c_str(), e.path.c_str());
    } else if (cmd == "BONES" && argc >= 4) {
        std::string want = lower(argv[3]); std::replace(want.begin(), want.end(), '\\', '/');
        if (want.empty() || want[0] != '/') want = "/" + want;
        for (size_t i = pk.size(); i-- > 0;) for (auto& e : pk[i]->ents) if (lower(e.path) == want) {
            std::vector<byte> d; if (!pk[i]->read(e.off, e.size, d)) { fprintf(stderr, "read failed\n"); return 1; }
            printf("%s from %s\n", e.path.c_str(), pk[i]->name.c_str());
            return bonesCmd(d, argc > 4 ? argv[4] : nullptr);
        }
        fprintf(stderr, "not found\n"); return 1;
    } else if (cmd == "GET" && argc >= 5) {
        std::string want = lower(argv[3]); std::replace(want.begin(), want.end(), '\\', '/');
        if (want.empty() || want[0] != '/') want = "/" + want;
        for (size_t i = pk.size(); i-- > 0;) for (auto& e : pk[i]->ents) if (lower(e.path) == want) {
            std::vector<byte> d; if (!pk[i]->read(e.off, e.size, d)) { fprintf(stderr, "read failed\n"); return 1; }
            FILE* o = fopen(argv[4], "wb"); fwrite(d.data(), 1, d.size(), o); fclose(o);
            printf("%s: %zu bytes from %s\n", e.path.c_str(), d.size(), pk[i]->name.c_str()); return 0;
        }
        fprintf(stderr, "not found\n"); return 1;
    } else return 2;
    return 0;
}
