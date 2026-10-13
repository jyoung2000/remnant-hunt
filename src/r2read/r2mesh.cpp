// r2read: Remnant II skeletal meshes, skeletons and animations, read in memory from the player's own install.
// Copyright (C) 2026 Jalon and contributors. GPL-3.0-or-later.
//
// Formats: UE 5.2 cooked Zen packages, following UE's own serializers and CUE4Parse (Apache-2.0). Unversioned
// properties are not parsed: the native part of an export is found by its reference skeleton (bone count, then FName
// and parent index per bone, the root's parent being -1), and everything after it is read in UE's fixed order.
// Animations are ACL-compressed (UE's ACL plugin); they are decoded with the Animation Compression Library (MIT).
#include "r2mesh.h"

#include <cmath>
#include <cstring>
#include <stdexcept>

#include <acl/decompression/decompress.h>

namespace r2 {

namespace {

struct Rd {
    const Bytes& d;
    size_t p;
    void need(size_t n) const {
        if (p + n > d.size()) throw std::runtime_error("read past the end at " + std::to_string(p));
    }
    template <class T> T get() {
        need(sizeof(T));
        T v;
        memcpy(&v, d.data() + p, sizeof(T));
        p += sizeof(T);
        return v;
    }
    bool b() {
        uint32_t v = get<uint32_t>();
        if (v > 1) throw std::runtime_error("bool expected at " + std::to_string(p - 4));
        return v == 1;
    }
    void skip(size_t n) { need(n); p += n; }
    int32_t count(size_t elem, size_t limit = 50000000) {
        int32_t n = get<int32_t>();
        if (n < 0 || (size_t)n > limit) throw std::runtime_error("bad count at " + std::to_string(p - 4));
        need((size_t)n * elem);
        return n;
    }
};

size_t headerSize(const Bytes& pkg) {
    if (pkg.size() < 8) throw std::runtime_error("not a package");
    uint32_t h;
    memcpy(&h, pkg.data() + 4, 4);
    return h;
}

// Bone list with its FName + parent pattern; returns the offset of the count.
size_t findRefSkeleton(const Bytes& d, size_t nNames, size_t from) {
    for (size_t p = from; p + 4 + 6 * 12 <= d.size(); p++) {
        int32_t n;
        memcpy(&n, d.data() + p, 4);
        if (n < 1 || n > 4096 || p + 4 + (size_t)n * 12 > d.size()) continue;
        bool ok = true;
        for (int k = 0; k < std::min(n, 6) && ok; k++) {
            uint32_t ni, num;
            int32_t par;
            memcpy(&ni, d.data() + p + 4 + k * 12, 4);
            memcpy(&num, d.data() + p + 8 + k * 12, 4);
            memcpy(&par, d.data() + p + 12 + k * 12, 4);
            ok = ni < nNames && num <= 64 && (k == 0 ? par == -1 : (par >= 0 && par < k));
        }
        if (ok) return p;
    }
    throw std::runtime_error("no reference skeleton");
}

void readSkeleton(Rd& r, const std::vector<std::string>& names, std::vector<Bone>* bones) {
    int32_t n = r.count(12, 4096);
    bones->resize(n);
    for (int i = 0; i < n; i++) {
        uint32_t ni = r.get<uint32_t>(), num = r.get<uint32_t>();
        int32_t par = r.get<int32_t>();
        if (ni >= names.size()) throw std::runtime_error("bone name out of range");
        (*bones)[i].name = names[ni] + (num ? "_" + std::to_string(num - 1) : "");
        (*bones)[i].parent = par;
    }
    int32_t np = r.count(80, 4096);
    for (int i = 0; i < np && i < n; i++) {
        double q[4], t[3], s[3];
        for (double& x : q) x = r.get<double>();
        for (double& x : t) x = r.get<double>();
        for (double& x : s) x = r.get<double>();
        for (int k = 0; k < 4; k++) (*bones)[i].rot[k] = (float)q[k];
        for (int k = 0; k < 3; k++) { (*bones)[i].pos[k] = (float)t[k]; (*bones)[i].scale[k] = (float)s[k]; }
    }
    int32_t nm = r.count(12, 4096);
    r.skip((size_t)nm * 12);
}

}  // namespace

bool readMesh(const Bytes& d, Mesh* out, std::string* err) {
    try {
        size_t nameEnd = 0;
        std::vector<std::string> nm = names(d, &nameEnd);
        size_t hs = headerSize(d);
        size_t sk = findRefSkeleton(d, nm.size(), hs);
        // Material slots sit right before the skeleton, 40 bytes each: {FPackageIndex, FName slot, UV channel data}.
        out->materials.clear();
        for (size_t p = sk; p >= hs + 40; p -= 40) {
            int32_t idx;
            uint32_t ni, num;
            memcpy(&idx, d.data() + p - 40, 4);
            memcpy(&ni, d.data() + p - 36, 4);
            memcpy(&num, d.data() + p - 32, 4);
            if (!(idx < 0 && idx > -4096 && ni < nm.size() && num == 0)) break;
            out->materials.insert(out->materials.begin(), nm[ni]);
        }
        Rd r{d, sk};
        readSkeleton(r, nm, &out->bones);
        // FSkeletalMeshRenderData (cooked)
        r.b();  // bCooked
        int32_t nLods = r.count(1, 64);
        for (int lod = 0; lod < nLods; lod++) {
            r.skip(2);                // strip flags
            bool cookedOut = r.b();
            bool inlined = r.b();
            int32_t nReq = r.count(2);
            r.skip((size_t)nReq * 2);
            if (cookedOut) continue;
            int32_t nSec = r.count(1, 256);
            std::vector<Section> secs(nSec);
            std::vector<std::vector<uint16_t>> boneMaps(nSec);
            for (int s = 0; s < nSec; s++) {
                r.get<uint8_t>();
                uint8_t classStrip = r.get<uint8_t>();
                secs[s].material = r.get<int16_t>();
                secs[s].baseIndex = r.get<uint32_t>();
                secs[s].numTriangles = r.get<uint32_t>();
                r.b();                  // bRecomputeTangent
                r.get<uint8_t>();       // RecomputeTangentsVertexMaskChannel
                r.b();                  // bCastShadow
                r.b();                  // bVisibleInRayTracing
                secs[s].baseVertex = r.get<uint32_t>();
                int32_t nCloth = r.count(4, 64);
                for (int c = 0; c < nCloth; c++) { int32_t m = r.count(36); r.skip((size_t)m * 36); }
                int32_t nMap = r.count(2, 65536);
                boneMaps[s].resize(nMap);
                for (int k = 0; k < nMap; k++) boneMaps[s][k] = r.get<uint16_t>();
                secs[s].numVertices = r.get<uint32_t>();
                r.get<int32_t>();       // max bone influences
                r.get<int16_t>();       // cloth asset index
                r.skip(20);             // clothing section data
                if (!(classStrip & 1)) {
                    int32_t a = r.count(4); r.skip((size_t)a * 4);
                    int32_t b2 = r.count(8); r.skip((size_t)b2 * 8);
                }
                r.b();                  // bDisabled
            }
            int32_t nAct = r.count(2);
            r.skip((size_t)nAct * 2);
            r.skip(4);                  // buffers size
            if (!inlined) {
                if (out->vertices.empty()) { *err = "LOD " + std::to_string(lod) + " is streamed"; return false; }
                break;
            }
            // streamed data, inline
            r.skip(2);
            uint8_t indexSize = r.get<uint8_t>();
            int32_t isz = r.get<int32_t>(), icount = r.count(indexSize);
            if (isz != indexSize) throw std::runtime_error("index size mismatch");
            std::vector<uint32_t> idx(icount);
            for (int i = 0; i < icount; i++) idx[i] = indexSize == 2 ? r.get<uint16_t>() : r.get<uint32_t>();
            r.get<int32_t>();           // stride
            int32_t nv = r.get<int32_t>();
            int32_t psz = r.get<int32_t>(), pcount = r.count(12);
            if (psz != 12 || pcount != nv) throw std::runtime_error("position buffer");
            std::vector<Vertex> v(nv);
            for (int i = 0; i < nv; i++)
                for (int k = 0; k < 3; k++) v[i].pos[k] = r.get<float>();
            r.skip(2);
            int32_t nTex = r.get<int32_t>();
            int32_t nv2 = r.get<int32_t>();
            bool fullUV = r.b(), highTan = r.b();
            if (nv2 != nv || nTex < 1 || nTex > 8) throw std::runtime_error("vertex buffer");
            int32_t tsz = r.get<int32_t>(), tcount = r.count(highTan ? 16 : 8);
            size_t tAt = r.p;
            r.skip((size_t)tsz * tcount);
            for (int i = 0; i < nv; i++) {
                if (highTan) {
                    int16_t z[4];
                    memcpy(z, d.data() + tAt + (size_t)i * 16 + 8, 8);
                    for (int k = 0; k < 3; k++) v[i].normal[k] = z[k];
                } else {
                    uint8_t z[4];
                    memcpy(z, d.data() + tAt + (size_t)i * 8 + 4, 4);
                    for (int k = 0; k < 3; k++) v[i].normal[k] = (int16_t)std::lround((z[k] - 127.5f) / 127.5f * 32767.0f);
                }
            }
            int32_t usz = r.get<int32_t>(), ucount = r.count(fullUV ? 8 : 4);
            size_t uAt = r.p;
            r.skip((size_t)usz * ucount);
            for (int i = 0; i < nv; i++) {
                if (fullUV) {
                    memcpy(v[i].uv, d.data() + uAt + (size_t)i * nTex * 8, 8);
                } else {
                    uint16_t h[2];
                    memcpy(h, d.data() + uAt + (size_t)i * nTex * 4, 4);
                    for (int k = 0; k < 2; k++) {
                        uint32_t s = (h[k] >> 15) & 1, e = (h[k] >> 10) & 31, m = h[k] & 1023;
                        float f = e == 0 ? std::ldexp((float)m, -24) : std::ldexp((float)(m | 1024), (int)e - 25);
                        v[i].uv[k] = s ? -f : f;
                    }
                }
            }
            // skin weights (new format: variable influences, 8/16-bit indices and weights)
            r.skip(2);
            bool variable = r.b();
            uint32_t maxInf = r.get<uint32_t>();
            r.get<uint32_t>();          // num bones
            uint32_t nvw = r.get<uint32_t>();
            bool idx16 = r.b(), w16 = r.b();
            int32_t wsz = r.get<int32_t>(), wcount = r.count(1);
            size_t wAt = r.p;
            r.skip((size_t)wsz * wcount);
            r.skip(2);
            r.get<int32_t>();           // lookup vertices
            int32_t lsz = r.get<int32_t>(), lcount = r.count(4);
            size_t lAt = r.p;
            r.skip((size_t)lsz * lcount);
            if (nvw != (uint32_t)nv || maxInf > 12) throw std::runtime_error("skin weights");
            size_t ib = idx16 ? 2 : 1, wb = w16 ? 2 : 1;
            // vertex -> section, for the section's bone map
            std::vector<int> secOf(nv, 0);
            for (int s = 0; s < nSec; s++)
                for (uint32_t k = 0; k < secs[s].numVertices && secs[s].baseVertex + k < (uint32_t)nv; k++)
                    secOf[secs[s].baseVertex + k] = s;
            for (int i = 0; i < nv; i++) {
                uint32_t off = (uint32_t)i * maxInf * (uint32_t)(ib + wb), n = maxInf;
                if (variable) {
                    uint32_t look;
                    memcpy(&look, d.data() + lAt + (size_t)i * 4, 4);
                    off = look >> 8;
                    n = look & 0xFF;
                }
                const uint8_t* q = d.data() + wAt + off;
                const std::vector<uint16_t>& bm = boneMaps[secOf[i]];
                for (uint32_t k = 0; k < n && k < 8; k++) {
                    uint32_t bi = idx16 ? (q[k * 2] | (q[k * 2 + 1] << 8)) : q[k];
                    uint32_t wi = w16 ? (q[n * ib + k * 2] | (q[n * ib + k * 2 + 1] << 8)) >> 8 : q[n * ib + k];
                    v[i].bones[k] = bi < bm.size() ? bm[bi] : 0;
                    v[i].weights[k] = (uint8_t)wi;
                }
            }
            out->sections = secs;
            out->vertices = std::move(v);
            out->indices = std::move(idx);
            break;  // LOD0 is all the mod draws for now
        }
        if (out->vertices.empty()) { *err = "no inline LOD"; return false; }
        return true;
    } catch (const std::exception& e) {
        *err = std::string("mesh: ") + e.what();
        return false;
    }
}

bool readSkeletonAsset(const Bytes& d, std::vector<Bone>* bones, std::string* err) {
    try {
        size_t nameEnd = 0;
        std::vector<std::string> nm = names(d, &nameEnd);
        Rd r{d, findRefSkeleton(d, nm.size(), headerSize(d))};
        readSkeleton(r, nm, bones);
        return true;
    } catch (const std::exception& e) {
        *err = std::string("skeleton: ") + e.what();
        return false;
    }
}

namespace {

struct UEDecompression : acl::default_transform_decompression_settings {
    static constexpr acl::compressed_tracks_version16 version_supported() { return acl::compressed_tracks_version16::any; }
};

struct Writer final : acl::track_writer {
    float* out;  // tracks x 10 floats
    void RTM_SIMD_CALL write_rotation(uint32_t t, rtm::quatf_arg0 q) {
        rtm::quat_store(q, out + t * 10);
    }
    void RTM_SIMD_CALL write_translation(uint32_t t, rtm::vector4f_arg0 v) {
        rtm::vector_store3(v, out + t * 10 + 4);
    }
    void RTM_SIMD_CALL write_scale(uint32_t t, rtm::vector4f_arg0 v) {
        rtm::vector_store3(v, out + t * 10 + 7);
    }
};

}  // namespace

bool readAnimation(const Bytes& d, Animation* out, std::string* err) {
    try {
        const uint8_t tag[4] = {0x11, 0xac, 0x11, 0xac};
        size_t at = 0;
        for (size_t p = 8; p + 32 <= d.size(); p++)
            if (!memcmp(d.data() + p, tag, 4)) { at = p - 8; break; }
        if (!at) { *err = "no ACL data (not an ACL-compressed sequence)"; return false; }
        uint32_t size;
        memcpy(&size, d.data() + at, 4);
        if (at + size > d.size()) { *err = "ACL data truncated"; return false; }
        // ACL needs 16-byte alignment
        std::vector<uint8_t> buf(size + 16);
        uint8_t* aligned = (uint8_t*)(((uintptr_t)buf.data() + 15) & ~(uintptr_t)15);
        memcpy(aligned, d.data() + at, size);
        acl::error_result e;
        const acl::compressed_tracks* ct = acl::make_compressed_tracks(aligned, &e);
        if (!ct) { *err = std::string("ACL: ") + (e.c_str() ? e.c_str() : "invalid"); return false; }
        uint32_t nTracks = ct->get_num_tracks(), nSamples = ct->get_num_samples_per_track();
        // The track -> skeleton bone table (int32 count, then the bone indexes) ends a few bytes before the ACL buffer.
        out->trackToBone.assign(nTracks, -1);
        for (size_t gap = 0; gap <= 16; gap += 4) {
            size_t need = gap + (size_t)nTracks * 4 + 4;
            if (at < need) break;
            size_t tbl = at - gap - (size_t)nTracks * 4;
            int32_t n;
            memcpy(&n, d.data() + tbl - 4, 4);
            if ((uint32_t)n != nTracks) continue;
            for (uint32_t i = 0; i < nTracks; i++) memcpy(&out->trackToBone[i], d.data() + tbl + i * 4, 4);
            break;
        }
        out->numTracks = nTracks;
        out->numSamples = nSamples;
        out->rate = ct->get_sample_rate();
        out->samples.assign((size_t)nTracks * nSamples * 10, 0.0f);
        acl::decompression_context<UEDecompression> ctx;
        if (!ctx.initialize(*ct)) { *err = "ACL: cannot decompress this version"; return false; }
        Writer w;
        for (uint32_t s = 0; s < nSamples; s++) {
            ctx.seek(nSamples > 1 ? (float)s / out->rate : 0.0f, acl::sample_rounding_policy::nearest);
            w.out = out->samples.data() + (size_t)s * nTracks * 10;
            for (uint32_t t = 0; t < nTracks; t++) w.out[t * 10 + 7] = w.out[t * 10 + 8] = w.out[t * 10 + 9] = 1.0f;
            ctx.decompress_tracks(w);
        }
        return true;
    } catch (const std::exception& e) {
        *err = std::string("anim: ") + e.what();
        return false;
    }
}

}  // namespace r2
