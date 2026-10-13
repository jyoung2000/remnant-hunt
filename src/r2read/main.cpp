// r2read: reads Remnant II's own packages in place for the Remnant x MHW mashup.
// Copyright (C) 2026 Jalon and contributors.
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
// Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option)
// any later version. It links ooz (Copyright (C) 2016 Powzix, GPL-3.0). Source: see the mashup's source repository.
//
// The mashup talks to this helper over stdin/stdout. It never writes game data anywhere: text and textures are
// decoded in memory from the player's own Remnant II install and handed to the running mashup.
//
// Requests (one line, tab-separated):     Responses:
//   HELLO                                  OK r2read 1 <files>
//   TEXT <asset> <32-hex key>              OK <bytes>\n<utf-8>
//   NAME <asset> <regex with one group>    OK <bytes>\n<utf-8>
//   TEX <asset without ext> <max side>     OK <BC7|BC3|BC1|BC5|BC4|BGRA8> <w> <h> <bytes>\n<pixels>
//   SND <event asset> <seed>               OK <rate> <frames> <bytes>\n<stereo 16-bit PCM>
//   MESH <skeletal mesh asset>             OK <bytes>\n<RMSH: bones, material slots, sections, vertices, indices>
//   SKEL <skeleton asset>                  OK <bytes>\n<RSKL: bones>
//   ANIM <anim sequence asset>             OK <bytes>\n<RANM: tracks, samples, rate, track->bone, samples>
//   QUIT
// Errors: ERR <message>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <regex>
#include <string>

#include "r2pak.h"
#include "wwise.h"
#include "r2mesh.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using namespace r2;

static Paks g_paks;

// Small cache: dialogue trees are asked for several lines in a row.
static std::list<std::pair<std::string, Bytes>> g_cache;
static const Bytes* package(const std::string& path, std::string* err) {
    for (auto it = g_cache.begin(); it != g_cache.end(); ++it)
        if (it->first == path) {
            g_cache.splice(g_cache.begin(), g_cache, it);
            return &g_cache.front().second;
        }
    Bytes b;
    if (!g_paks.read(path, &b, err)) return nullptr;
    g_cache.emplace_front(path, std::move(b));
    if (g_cache.size() > 24) g_cache.pop_back();
    return &g_cache.front().second;
}

static void reply(const std::string& head, const void* data = nullptr, size_t n = 0) {
    fwrite(head.data(), 1, head.size(), stdout);
    fputc('\n', stdout);
    if (n) fwrite(data, 1, n, stdout);
    fflush(stdout);
}

static void replyText(const std::string& s) { reply("OK " + std::to_string(s.size()), s.data(), s.size()); }

static std::string fmtName(const std::string& pf) {
    static const std::map<std::string, std::string> m = {{"PF_BC7", "BC7"}, {"PF_DXT5", "BC3"}, {"PF_DXT1", "BC1"},
                                                          {"PF_BC5", "BC5"}, {"PF_BC4", "BC4"}, {"PF_B8G8R8A8", "BGRA8"}};
    auto it = m.find(pf);
    return it == m.end() ? pf : it->second;
}

// Little binary writer for the MESH / SKEL / ANIM replies.
struct Out {
    std::string b;
    template <class T> void put(const T& v) { b.append((const char*)&v, sizeof(T)); }
    void str(const std::string& s) { put((uint16_t)s.size()); b += s; }
    void bones(const std::vector<Bone>& bs) {
        put((uint32_t)bs.size());
        for (const Bone& x : bs) {
            str(x.name);
            put(x.parent);
            b.append((const char*)x.rot, 16);
            b.append((const char*)x.pos, 12);
            b.append((const char*)x.scale, 12);
        }
    }
};

static void handle(const std::string& line) {
    std::vector<std::string> f;
    size_t s = 0;
    for (size_t i = 0; i <= line.size(); i++)
        if (i == line.size() || line[i] == '\t') { f.push_back(line.substr(s, i - s)); s = i + 1; }
    std::string err;
    if (f[0] == "HELLO") {
        reply("OK r2read 1 " + std::to_string(g_paks.fileCount()));
    } else if (f[0] == "TEXT" && f.size() == 3) {
        const Bytes* p = package(f[1], &err);
        if (!p) return reply("ERR " + err);
        for (const Text& t : texts(*p))
            if (t.key == f[2]) return replyText(t.source);
        reply("ERR key not found");
    } else if (f[0] == "NAME" && f.size() == 3) {
        const Bytes* p = package(f[1], &err);
        if (!p) return reply("ERR " + err);
        std::regex re(f[2]);
        std::smatch m;
        for (const std::string& n : names(*p))
            if (std::regex_match(n, m, re) && m.size() > 1) return replyText(m[1].str());
        reply("ERR name not found");
    } else if (f[0] == "TEX" && f.size() == 3) {
        const Bytes* p = package(f[1] + ".uasset", &err);
        if (!p) return reply("ERR " + err);
        Texture t;
        if (!parseTexture(*p, &t, &err)) return reply("ERR " + err);
        int maxSide = atoi(f[2].c_str());
        size_t pick = 0;
        for (size_t i = 0; i < t.mips.size(); i++) {
            pick = i;
            if (std::max(t.mips[i].w, t.mips[i].h) <= maxSide) break;
        }
        const Mip& m = t.mips[pick];
        Bytes px;
        if (m.inlineData) {
            px.assign(p->begin() + m.at, p->begin() + m.at + m.size);
        } else {
            Bytes bulk;
            if (!g_paks.read(f[1] + ".ubulk", &bulk, &err)) return reply("ERR " + err);
            if (m.bulkOffset < 0 || (size_t)(m.bulkOffset + m.size) > bulk.size()) return reply("ERR bulk range");
            px.assign(bulk.begin() + m.bulkOffset, bulk.begin() + m.bulkOffset + m.size);
        }
        char head[96];
        snprintf(head, sizeof(head), "OK %s %d %d %zu", fmtName(t.format).c_str(), m.w, m.h, px.size());
        reply(head, px.data(), px.size());
    } else if (f[0] == "SND" && f.size() == 3) {
        const Bytes* p = package(f[1], &err);
        if (!p) return reply("ERR " + err);
        std::vector<int16_t> pcm;
        int rate = 0;
        if (!eventPcm(g_paks, *p, (uint32_t)strtoul(f[2].c_str(), nullptr, 10), &pcm, &rate, &err))
            return reply("ERR " + err);
        char head[96];
        snprintf(head, sizeof(head), "OK %d %zu %zu", rate, pcm.size() / 2, pcm.size() * 2);
        reply(head, pcm.data(), pcm.size() * 2);
    } else if ((f[0] == "MESH" || f[0] == "SKEL" || f[0] == "ANIM") && f.size() == 2) {
        const Bytes* p = package(f[1], &err);
        if (!p) return reply("ERR " + err);
        Out o;
        if (f[0] == "MESH") {
            Mesh m;
            if (!readMesh(*p, &m, &err)) return reply("ERR " + err);
            o.b = "RMSH";
            o.bones(m.bones);
            o.put((uint32_t)m.materials.size());
            for (const std::string& s : m.materials) o.str(s);
            o.put((uint32_t)m.sections.size());
            for (const Section& s : m.sections) o.put(s);
            o.put((uint32_t)m.vertices.size());
            o.b.append((const char*)m.vertices.data(), m.vertices.size() * sizeof(Vertex));
            o.put((uint32_t)m.indices.size());
            o.b.append((const char*)m.indices.data(), m.indices.size() * 4);
        } else if (f[0] == "SKEL") {
            std::vector<Bone> bs;
            if (!readSkeletonAsset(*p, &bs, &err)) return reply("ERR " + err);
            o.b = "RSKL";
            o.bones(bs);
        } else {
            Animation a;
            if (!readAnimation(*p, &a, &err)) return reply("ERR " + err);
            o.b = "RANM";
            o.put(a.numTracks);
            o.put(a.numSamples);
            o.put(a.rate);
            o.b.append((const char*)a.trackToBone.data(), a.trackToBone.size() * 4);
            o.b.append((const char*)a.samples.data(), a.samples.size() * 4);
        }
        replyText(o.b);
    } else {
        reply("ERR bad request");
    }
}

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    if (argc < 2) {
        fprintf(stderr, "usage: r2read <Remnant II install folder or its Paks folder>\n");
        return 2;
    }
    std::string dir = argv[1];
    std::string err;
    if (!g_paks.open(dir + "/Remnant2/Content/Paks", &err) && !g_paks.open(dir, &err)) {
        printf("ERR %s\n", err.c_str());
        fflush(stdout);
        return 1;
    }
    char buf[8192];
    while (fgets(buf, sizeof(buf), stdin)) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line == "QUIT") break;
        if (!line.empty()) handle(line);
    }
    return 0;
}
