// Remnant x MHW mashup core: Remnant II models and animations decoded from r2read's replies. GPL-3.0-or-later.
#include "model.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include "core.h"

namespace rh {

namespace {

struct In {
    const std::vector<uint8_t>& d;
    size_t p = 0;
    bool ok = true;
    template <class T> T get() {
        T v{};
        if (p + sizeof(T) > d.size()) { ok = false; return v; }
        memcpy(&v, d.data() + p, sizeof(T));
        p += sizeof(T);
        return v;
    }
    std::string str() {
        uint16_t n = get<uint16_t>();
        if (p + n > d.size()) { ok = false; return {}; }
        std::string s((const char*)d.data() + p, n);
        p += n;
        return s;
    }
    void bones(std::vector<ModelBone>* out) {
        uint32_t n = get<uint32_t>();
        if (n > 4096) { ok = false; return; }
        out->resize(n);
        for (auto& b : *out) {
            b.name = str();
            b.parent = get<int32_t>();
            for (float& x : b.ref.q) x = get<float>();
            for (float& x : b.ref.t) x = get<float>();
            for (float& x : b.ref.s) x = get<float>();
        }
    }
};

// 3x4 row-major affine matrices: p' = M * [p 1]
void to_mat(const Xform& x, float m[12]) {
    float qx = x.q[0], qy = x.q[1], qz = x.q[2], qw = x.q[3];
    float r[9] = {1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw),
                  2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw),
                  2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) m[i * 4 + j] = r[i * 3 + j] * x.s[j];
        m[i * 4 + 3] = x.t[i];
    }
}
void mul(const float a[12], const float b[12], float out[12]) {
    float o[12];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) o[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j];
        o[i * 4 + 3] = a[i * 4] * b[3] + a[i * 4 + 1] * b[7] + a[i * 4 + 2] * b[11] + a[i * 4 + 3];
    }
    memcpy(out, o, sizeof(o));
}
void inverse(const float m[12], float out[12]) {
    // general 3x3 inverse + translation
    float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    float det = a * A + b * B + c * C;
    float k = fabsf(det) > 1e-12f ? 1.0f / det : 0.0f;
    float r[9] = {A * k, -(b * i - c * h) * k, (b * f - c * e) * k,
                  B * k, (a * i - c * g) * k, -(a * f - c * d) * k,
                  C * k, -(a * h - b * g) * k, (a * e - b * d) * k};
    for (int x = 0; x < 3; x++) {
        for (int y = 0; y < 3; y++) out[x * 4 + y] = r[x * 3 + y];
        out[x * 4 + 3] = -(r[x * 3] * m[3] + r[x * 3 + 1] * m[7] + r[x * 3 + 2] * m[11]);
    }
}

std::mutex g_lock;
std::map<std::string, std::unique_ptr<Model>> g_models;
std::map<std::string, std::unique_ptr<Anim>> g_anims;
std::map<std::string, bool> g_failed;

}  // namespace

const Model* model_get(const char* asset, bool* failed) {
    std::lock_guard<std::mutex> l(g_lock);
    std::string key(asset ? asset : "");
    if (g_failed.count("M" + key)) {  // decoded once and failed: say so once, not every frame
        if (failed) *failed = true;
        return nullptr;
    }
    if (failed) *failed = false;
    auto it = g_models.find(key);
    if (it != g_models.end()) return it->second.get();
    const R2Blob* b = r2_blob("MESH", asset);
    if (!b) return nullptr;
    In in{b->data};
    auto m = std::make_unique<Model>();
    if (b->failed || b->data.size() < 4 || memcmp(b->data.data(), "RMSH", 4)) in.ok = false;
    in.p = 4;
    if (in.ok) in.bones(&m->bones);
    uint32_t nm = in.get<uint32_t>();
    for (uint32_t i = 0; in.ok && i < nm && i < 256; i++) m->materials.push_back(in.str());
    uint32_t ns = in.get<uint32_t>();
    for (uint32_t i = 0; in.ok && i < ns && i < 256; i++) m->sections.push_back(in.get<ModelSection>());
    uint32_t nv = in.get<uint32_t>();
    if (in.ok && in.p + (size_t)nv * sizeof(ModelVertex) <= b->data.size()) {
        m->vertices.resize(nv);
        memcpy(m->vertices.data(), b->data.data() + in.p, (size_t)nv * sizeof(ModelVertex));
        in.p += (size_t)nv * sizeof(ModelVertex);
    } else {
        in.ok = false;
    }
    uint32_t ni = in.get<uint32_t>();
    if (in.ok && in.p + (size_t)ni * 4 <= b->data.size()) {
        m->indices.resize(ni);
        memcpy(m->indices.data(), b->data.data() + in.p, (size_t)ni * 4);
    } else {
        in.ok = false;
    }
    if (!in.ok || m->bones.empty() || m->vertices.empty()) {
        g_failed["M" + key] = true;
        if (failed) *failed = true;
        log("model: %s could not be decoded", asset);
        return nullptr;
    }
    // inverse bind matrices
    size_t n = m->bones.size();
    std::vector<float> global(n * 12);
    m->invBind.resize(n * 12);
    for (size_t i = 0; i < n; i++) {
        float local[12];
        to_mat(m->bones[i].ref, local);
        int p = m->bones[i].parent;
        if (p >= 0 && (size_t)p < i) mul(&global[p * 12], local, &global[i * 12]);
        else memcpy(&global[i * 12], local, sizeof(local));
        inverse(&global[i * 12], &m->invBind[i * 12]);
    }
    // The body's extent without stray far vertices (effect planes, detached bits): 1st and 99th percentiles.
    {
        std::vector<float> axis(m->vertices.size());
        for (int k = 0; k < 3; k++) {
            for (size_t i = 0; i < axis.size(); i++) axis[i] = m->vertices[i].pos[k];
            size_t lo = axis.size() / 100, hi = axis.size() - 1 - axis.size() / 100;
            std::nth_element(axis.begin(), axis.begin() + lo, axis.end());
            m->boundsMin[k] = axis[lo];
            std::nth_element(axis.begin(), axis.begin() + hi, axis.end());
            m->boundsMax[k] = axis[hi];
        }
    }
    log("model: %s: %zu bones, %zu vertices, %zu triangles, %zu materials", asset, n, m->vertices.size(),
        m->indices.size() / 3, m->materials.size());
    const Model* r = m.get();
    g_models[key] = std::move(m);
    return r;
}

const Model* skeleton_model(const char* asset, bool* failed) {
    std::lock_guard<std::mutex> l(g_lock);
    std::string key = std::string("S|") + (asset ? asset : "");
    if (failed) *failed = g_failed.count(key) > 0;
    auto it = g_models.find(key);
    if (it != g_models.end()) return it->second.get();
    if (failed && *failed) return nullptr;
    const R2Blob* b = r2_blob("SKEL", asset);
    if (!b) return nullptr;
    auto m = std::make_unique<Model>();
    In in{b->data};
    if (b->failed || b->data.size() < 8 || memcmp(b->data.data(), "RSKL", 4)) in.ok = false;
    in.p = 4;
    if (in.ok) in.bones(&m->bones);
    if (!in.ok || m->bones.empty()) {
        g_failed[key] = true;
        if (failed) *failed = true;
        log("model: skeleton %s could not be decoded", asset);
        return nullptr;
    }
    size_t n = m->bones.size();
    std::vector<float> global(n * 12);
    m->invBind.resize(n * 12);
    for (size_t i = 0; i < n; i++) {
        float local[12];
        to_mat(m->bones[i].ref, local);
        int p = m->bones[i].parent;
        if (p >= 0 && (size_t)p < i) mul(&global[p * 12], local, &global[i * 12]);
        else memcpy(&global[i * 12], local, sizeof(local));
        inverse(&global[i * 12], &m->invBind[i * 12]);
    }
    log("model: skeleton %s: %zu bones", asset, n);
    const Model* r = m.get();
    g_models[key] = std::move(m);
    return r;
}

const Anim* anim_get(const char* animAsset, const char* skelAsset, const Model* m, bool* failed) {
    if (!m) return nullptr;
    std::lock_guard<std::mutex> l(g_lock);
    std::string key = std::string(animAsset ? animAsset : "") + "|" + std::to_string((uintptr_t)m);
    if (failed) *failed = g_failed.count("A" + key) > 0;
    auto it = g_anims.find(key);
    if (it != g_anims.end()) return it->second.get();
    if (failed && *failed) return nullptr;
    const R2Blob* ab = r2_blob("ANIM", animAsset);
    const R2Blob* sb = r2_blob("SKEL", skelAsset);
    if (!ab || !sb) return nullptr;
    auto a = std::make_unique<Anim>();
    bool ok = !ab->failed && !sb->failed && ab->data.size() > 16 && !memcmp(ab->data.data(), "RANM", 4) &&
              sb->data.size() > 8 && !memcmp(sb->data.data(), "RSKL", 4);
    std::vector<ModelBone> skel;
    if (ok) {
        In s{sb->data};
        s.p = 4;
        s.bones(&skel);
        In in{ab->data};
        in.p = 4;
        a->tracks = in.get<uint32_t>();
        a->samples = in.get<uint32_t>();
        a->rate = in.get<float>();
        size_t need = (size_t)a->tracks * 4 + (size_t)a->tracks * a->samples * 40;
        ok = s.ok && in.ok && a->tracks > 0 && a->tracks <= 4096 && a->samples > 0 && a->rate > 0 && in.p + need <= ab->data.size();
        if (ok) {
            a->trackToSkelBone.resize(a->tracks);
            memcpy(a->trackToSkelBone.data(), ab->data.data() + in.p, (size_t)a->tracks * 4);
            in.p += (size_t)a->tracks * 4;
            a->data.resize((size_t)a->tracks * a->samples * 10);
            memcpy(a->data.data(), ab->data.data() + in.p, a->data.size() * 4);
        }
    }
    if (!ok) {
        g_failed["A" + key] = true;
        if (failed) *failed = true;
        log("anim: %s could not be decoded", animAsset);
        return nullptr;
    }
    std::map<std::string, int> byName;
    for (size_t i = 0; i < m->bones.size(); i++) byName[m->bones[i].name] = (int)i;
    a->trackToModelBone.assign(a->tracks, -1);
    int bound = 0;
    for (uint32_t t = 0; t < a->tracks; t++) {
        int sbn = a->trackToSkelBone[t];
        if (sbn >= 0 && (size_t)sbn < skel.size()) {
            auto f = byName.find(skel[sbn].name);
            if (f != byName.end()) { a->trackToModelBone[t] = f->second; bound++; }
        }
    }
    if (bound == 0) {
        g_failed["A" + key] = true;
        if (failed) *failed = true;
        log("anim: %s has no tracks mapped to the requested skeleton", animAsset);
        return nullptr;
    }
    log("anim: %s: %u tracks (%d on the model), %u samples at %.0f fps", animAsset, a->tracks, bound, a->samples, a->rate);
    const Anim* r = a.get();
    g_anims[key] = std::move(a);
    return r;
}

static void sample(const Anim& a, float t, bool loop, std::vector<Xform>* local, std::vector<char>* has) {
    float len = a.length();
    if (loop && len > 0) t = fmodf(t, len);
    if (t < 0) t = 0;
    float f = t * a.rate;
    uint32_t i0 = std::min((uint32_t)f, a.samples - 1), i1 = std::min(i0 + 1, a.samples - 1);
    float k = std::min(1.0f, std::max(0.0f, f - (float)i0));
    for (uint32_t tr = 0; tr < a.tracks; tr++) {
        int b = a.trackToModelBone[tr];
        if (b < 0 || (size_t)b >= local->size()) continue;
        const float* x0 = &a.data[((size_t)i0 * a.tracks + tr) * 10];
        const float* x1 = &a.data[((size_t)i1 * a.tracks + tr) * 10];
        Xform& o = (*local)[b];
        float dot = x0[0] * x1[0] + x0[1] * x1[1] + x0[2] * x1[2] + x0[3] * x1[3];
        float s1 = dot < 0 ? -k : k, n = 0;
        for (int c = 0; c < 4; c++) { o.q[c] = x0[c] * (1 - k) + x1[c] * s1; n += o.q[c] * o.q[c]; }
        n = n > 0 ? 1.0f / sqrtf(n) : 1.0f;
        for (int c = 0; c < 4; c++) o.q[c] *= n;
        for (int c = 0; c < 3; c++) {
            o.t[c] = x0[4 + c] * (1 - k) + x1[4 + c] * k;
            o.s[c] = x0[7 + c] * (1 - k) + x1[7 + c] * k;
        }
        (*has)[b] = 1;
    }
}

bool anim_root(const Anim& a, float t, bool loop, float out[3]) {
    int tr = -1;
    for (uint32_t i = 0; i < a.tracks; i++)
        if (a.trackToModelBone[i] == 0) { tr = (int)i; break; }
    if (tr < 0 || a.samples == 0) { out[0] = out[1] = out[2] = 0; return false; }
    float len = a.length();
    if (loop && len > 0) t = fmodf(t, len);
    t = std::max(0.0f, std::min(t, len));
    float f = t * a.rate;
    uint32_t i0 = std::min((uint32_t)f, a.samples - 1), i1 = std::min(i0 + 1, a.samples - 1);
    float k = std::min(1.0f, std::max(0.0f, f - (float)i0));
    const float* x0 = &a.data[((size_t)i0 * a.tracks + tr) * 10];
    const float* x1 = &a.data[((size_t)i1 * a.tracks + tr) * 10];
    for (int c = 0; c < 3; c++) out[c] = x0[4 + c] * (1 - k) + x1[4 + c] * k;
    return true;
}

void pose(const Model& m, const Anim* a, float t, bool loop, const Anim* b, float tb, float blend, std::vector<float>* palette,
          float rootDelta[3]) {
    size_t n = m.bones.size();
    std::vector<Xform> la(n), lb;
    std::vector<char> ha(n, 0), hb;
    for (size_t i = 0; i < n; i++) la[i] = m.bones[i].ref;
    if (a) sample(*a, t, loop, &la, &ha);
    if (b && blend > 0) {
        lb = std::vector<Xform>(n);
        hb.assign(n, 0);
        for (size_t i = 0; i < n; i++) lb[i] = m.bones[i].ref;
        sample(*b, tb, loop, &lb, &hb);
        for (size_t i = 0; i < n; i++) {
            Xform& o = la[i];
            const Xform& x = lb[i];
            float dot = o.q[0] * x.q[0] + o.q[1] * x.q[1] + o.q[2] * x.q[2] + o.q[3] * x.q[3];
            float s1 = dot < 0 ? -blend : blend, nn = 0;
            for (int c = 0; c < 4; c++) { o.q[c] = o.q[c] * (1 - blend) + x.q[c] * s1; nn += o.q[c] * o.q[c]; }
            nn = nn > 0 ? 1.0f / sqrtf(nn) : 1.0f;
            for (int c = 0; c < 4; c++) o.q[c] *= nn;
            for (int c = 0; c < 3; c++) { o.t[c] += (x.t[c] - o.t[c]) * blend; o.s[c] += (x.s[c] - o.s[c]) * blend; }
        }
    }
    // Root motion: the creature's movement is driven by the game logic, so the root stays in place.
    if (n > 0) {
        if (rootDelta) for (int c = 0; c < 3; c++) rootDelta[c] = la[0].t[c] - m.bones[0].ref.t[c];
        la[0].t[0] = m.bones[0].ref.t[0];
        la[0].t[1] = m.bones[0].ref.t[1];
    }
    palette->resize(n * 12);
    std::vector<float> global(n * 12);
    for (size_t i = 0; i < n; i++) {
        float local[12];
        to_mat(la[i], local);
        int p = m.bones[i].parent;
        if (p >= 0 && (size_t)p < i) mul(&global[p * 12], local, &global[i * 12]);
        else memcpy(&global[i * 12], local, sizeof(local));
        mul(&global[i * 12], &m.invBind[i * 12], &(*palette)[i * 12]);
    }
}

void pose_global(const Model& m, const Anim* a, float t, bool loop, std::vector<float>* global) {
    size_t n = m.bones.size();
    std::vector<Xform> la(n);
    std::vector<char> ha(n, 0);
    for (size_t i = 0; i < n; i++) la[i] = m.bones[i].ref;
    if (a) sample(*a, t, loop, &la, &ha);
    if (n > 0) {
        la[0].t[0] = m.bones[0].ref.t[0];
        la[0].t[1] = m.bones[0].ref.t[1];
    }
    global->resize(n * 12);
    for (size_t i = 0; i < n; i++) {
        float local[12];
        to_mat(la[i], local);
        int p = m.bones[i].parent;
        if (p >= 0 && (size_t)p < i) mul(&(*global)[p * 12], local, &(*global)[i * 12]);
        else memcpy(&(*global)[i * 12], local, sizeof(local));
    }
}

}  // namespace rh
