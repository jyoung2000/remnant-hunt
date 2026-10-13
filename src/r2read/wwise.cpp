// r2read: Remnant II's Wwise sounds, decoded in memory for the Remnant x MHW mashup.
// Copyright (C) 2026 Jalon and contributors. GPL-3.0-or-later.
//
// Remnant II packs each sound event as a package holding its own small soundbank (BKHD/HIRC). The bank lists the
// sounds the event plays: groups of variations (random containers), layered together. Each sound's media is a
// Wwise Vorbis .wem in WwiseAudio/Media/<source id>.ubulk. We rebuild a standard Ogg Vorbis stream from it with
// ww2ogg (BSD-3, third_party/ww2ogg) and decode that with stb_vorbis (public domain, third_party/stb).
#include "wwise.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

#include "codebooks.inc"  // kCodebooks / kCodebooksSize: ww2ogg's packed_codebooks_aoTuV_603.bin
#include "wwriff.h"

extern "C" int stb_vorbis_decode_memory(const unsigned char* mem, int len, int* channels, int* sample_rate, short** output);

namespace r2 {

static uint32_t u32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

bool eventLayers(const Bytes& pkg, std::vector<std::vector<uint32_t>>* layers, std::string* err) {
    layers->clear();
    const uint8_t* b = pkg.data();
    size_t n = pkg.size();
    size_t p = 0;
    for (; p + 8 <= n; p++)
        if (!memcmp(b + p, "BKHD", 4) && p + 8 + u32(b + p + 4) <= n) break;
    if (p + 8 > n) { *err = "no soundbank in the event"; return false; }
    std::vector<uint32_t> cur;
    std::set<std::vector<uint32_t>> seen;
    auto close = [&] {
        if (cur.empty()) return;
        std::vector<uint32_t> key = cur;
        std::sort(key.begin(), key.end());
        if (seen.insert(key).second) layers->push_back(cur);  // the same set again is another path to the same layer
        cur.clear();
    };
    while (p + 8 <= n) {
        uint32_t sz = u32(b + p + 4);
        if (p + 8 + sz > n) break;
        if (!memcmp(b + p, "HIRC", 4)) {
            uint32_t count = u32(b + p + 8);
            size_t q = p + 12, end = p + 8 + sz;
            for (uint32_t i = 0; i < count && q + 9 <= end; i++) {
                uint8_t type = b[q];
                uint32_t len = u32(b + q + 1);
                const uint8_t* body = b + q + 9;  // after the object id
                if (type == 2 && len >= 4 + 13) {
                    // CAkSound: plugin id u32 (0x00040001 = Vorbis), stream type u8, source id u32, media size u32
                    uint32_t plugin = u32(body);
                    uint32_t source = u32(body + 5);
                    if (plugin == 0x00040001) cur.push_back(source);  // Vorbis only (all of Remnant II's sound effects)
                } else {
                    close();
                }
                q += 5 + len;
            }
            close();
        }
        p += 8 + sz;
    }
    if (layers->empty()) { *err = "the event has no Vorbis sounds"; return false; }
    return true;
}

bool wemToPcm(const Bytes& wem, std::vector<int16_t>* pcm, int* channels, int* rate, std::string* err) {
    try {
        std::string in((const char*)wem.data(), wem.size());
        std::string cb((const char*)kCodebooks, kCodebooksSize);
        Wwise_RIFF_Vorbis ww(in, cb, false, false, kNoForcePacketFormat);
        std::ostringstream os(std::ios::binary);
        ww.generate_ogg(os);
        std::string ogg = os.str();
        short* out = nullptr;
        int c = 0, r = 0;
        int frames = stb_vorbis_decode_memory((const unsigned char*)ogg.data(), (int)ogg.size(), &c, &r, &out);
        if (frames <= 0 || !out || c <= 0) {
            if (out) free(out);
            *err = "vorbis decode failed";
            return false;
        }
        pcm->assign(out, out + (size_t)frames * c);
        free(out);
        *channels = c;
        *rate = r;
        return true;
    } catch (const Parse_error& e) {
        std::ostringstream m;
        e.print_self(m);
        *err = "wem: " + m.str();
    } catch (const std::exception& e) {
        *err = std::string("wem: ") + e.what();
    } catch (...) {
        *err = "wem: unknown error";
    }
    return false;
}

// One play of an event: a random variation of every layer, mixed to stereo at the first layer's rate.
bool eventPcm(const Paks& paks, const Bytes& eventPkg, uint32_t seed, std::vector<int16_t>* out, int* rate,
              std::string* err) {
    std::vector<std::vector<uint32_t>> layers;
    if (!eventLayers(eventPkg, &layers, err)) return false;
    std::vector<float> mix;
    int mixRate = 0;
    uint32_t s = seed * 2654435761u + 12345u;
    int used = 0;
    for (const auto& layer : layers) {
        s = s * 1664525u + 1013904223u;
        uint32_t source = layer[(s >> 8) % layer.size()];
        Bytes wem;
        std::string e2;
        std::string base = "Remnant2/Content/WwiseAudio/Media/" + std::to_string(source);
        if (!paks.read(base + ".ubulk", &wem, &e2) && !paks.read(base + ".uasset", &wem, &e2)) continue;
        size_t riff = 0;
        while (riff + 4 <= wem.size() && memcmp(wem.data() + riff, "RIFF", 4)) riff++;
        if (riff + 12 > wem.size()) continue;
        if (riff) wem.erase(wem.begin(), wem.begin() + riff);
        std::vector<int16_t> pcm;
        int ch = 0, r = 0;
        if (!wemToPcm(wem, &pcm, &ch, &r, &e2)) {
            *err = e2;
            continue;
        }
        if (!mixRate) mixRate = r;
        if (r != mixRate) continue;
        size_t frames = pcm.size() / ch;
        if (mix.size() < frames * 2) mix.resize(frames * 2, 0.0f);
        for (size_t f = 0; f < frames; f++) {
            float l = pcm[f * ch] / 32768.0f, rr = ch > 1 ? pcm[f * ch + 1] / 32768.0f : l;
            mix[f * 2] += l;
            mix[f * 2 + 1] += rr;
        }
        used++;
    }
    if (!used) {
        if (err->empty()) *err = "no media for the event";
        return false;
    }
    float peak = 0;
    for (float v : mix) peak = std::max(peak, std::fabs(v));
    float k = peak > 0.9f ? 0.9f / peak : 1.0f;
    out->resize(mix.size());
    for (size_t i = 0; i < mix.size(); i++) (*out)[i] = (int16_t)std::lround(std::max(-1.0f, std::min(1.0f, mix[i] * k)) * 32767.0f);
    *rate = mixRate;
    return true;
}

}  // namespace r2
