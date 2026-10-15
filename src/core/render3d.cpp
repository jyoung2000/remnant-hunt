// Remnant x MHW mashup core: drawing Remnant II's own 3D models into Monster Hunter: World's frame (Direct3D 11).
// GPL-3.0-or-later (see LICENSE).
//
// Each frame, just before the overlay, the creatures and props the mod runs are drawn into the back buffer with
// MHW's own camera (view and projection read from the game). Skinning runs on the GPU from a bone palette the
// animation code fills. The mod keeps its own depth buffer, and where it can find MHW's scene depth it also hides
// what MHW's world puts in front.
#include "render3d.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>

#include <d3d11.h>
#include <d3dcompiler.h>

#include "core.h"

namespace rh {

static const char* kShader = R"(
cbuffer Frame : register(b0) {
    row_major float4x4 world;
    row_major float4x4 viewProj;
    float4 lightDir;   // world space, towards the light
    float4 tint;       // rgb multiply, a = emissive boost
    float4 depthInfo;  // x = 1 when MHW's depth is bound (2: and it runs opposite to this pass), y = this pass is reversed z, z/w = depth size / viewport size
    float4 camPos;     // world space
    float4 scene;      // x = sun strength, y = ambient, z = gain (calibrated to MHW's frame), w = 1: encode to gamma on output
    float4 ambTint;    // rgb = colour of MHW's frame under the boss, normalised
};
cbuffer Bones : register(b1) { float4 bones[3 * 1024]; };
Texture2D diffuseTex : register(t0);
Texture2D gameDepth : register(t1);
SamplerState samp : register(s0);
struct VSIn {
    float3 pos : POSITION; float4 nrm : NORMAL; float2 uv : TEXCOORD0;
    uint4 bi0 : BLENDINDICES0; uint4 bi1 : BLENDINDICES1; float4 bw0 : BLENDWEIGHT0; float4 bw1 : BLENDWEIGHT1;
};
struct PSIn { float4 pos : SV_Position; float3 nrm : NORMAL; float2 uv : TEXCOORD0; float3 wpos : TEXCOORD1; };
void add(uint i, float w, float3 p, float3 n, inout float3 op, inout float3 on) {
    if (w <= 0) return;
    float3x4 B = float3x4(bones[i * 3], bones[i * 3 + 1], bones[i * 3 + 2]);
    op += w * mul(B, float4(p, 1));
    on += w * mul((float3x3)B, n);
}
PSIn vs_main(VSIn v) {
    float3 p = 0, n = 0;
    add(v.bi0.x, v.bw0.x, v.pos, v.nrm.xyz, p, n); add(v.bi0.y, v.bw0.y, v.pos, v.nrm.xyz, p, n);
    add(v.bi0.z, v.bw0.z, v.pos, v.nrm.xyz, p, n); add(v.bi0.w, v.bw0.w, v.pos, v.nrm.xyz, p, n);
    add(v.bi1.x, v.bw1.x, v.pos, v.nrm.xyz, p, n); add(v.bi1.y, v.bw1.y, v.pos, v.nrm.xyz, p, n);
    add(v.bi1.z, v.bw1.z, v.pos, v.nrm.xyz, p, n); add(v.bi1.w, v.bw1.w, v.pos, v.nrm.xyz, p, n);
    float wsum = v.bw0.x + v.bw0.y + v.bw0.z + v.bw0.w + v.bw1.x + v.bw1.y + v.bw1.z + v.bw1.w;
    if (wsum < 0.01) { p = v.pos; n = v.nrm.xyz; }
    PSIn o;
    float4 wp = mul(float4(p, 1), world);
    o.pos = mul(wp, viewProj);
    o.nrm = mul(n, (float3x3)world);  // normalised per pixel (zero = no normal in the mesh)
    o.uv = v.uv;
    o.wpos = wp.xyz;
    return o;
}
float4 ps_main(PSIn i) : SV_Target {
    if (depthInfo.x > 0.5) {
        float g = gameDepth.Load(int3(i.pos.xy * depthInfo.zw, 0)).r;
        if (depthInfo.x > 1.5) g = 1.0 - g;  // MHW's buffer runs opposite to this pass's projection
        bool hidden = depthInfo.y > 0.5 ? (g > i.pos.z + 0.00002) : (g < i.pos.z - 0.00002);
        if (hidden) discard;
    }
    float4 c = diffuseTex.Sample(samp, i.uv);
    // normal: the mesh's, or a flat one from the screen-space derivatives of the position
    float3 n = i.nrm;
    float3 fn = cross(ddx(i.wpos), ddy(i.wpos));  // outside the branch: derivatives need uniform flow
    if (dot(n, n) < 1e-6) n = fn;
    n = normalize(n);
    float3 V = normalize(camPos.xyz - i.wpos);
    if (dot(n, V) < 0) n = -n;  // two-sided: back faces are lit from their visible side
    float3 L = normalize(lightDir.xyz);
    float ndl = saturate(dot(n, L));
    // sun: Lambert diffuse + a small Blinn-Phong highlight
    float3 sunCol = float3(1.0, 0.95, 0.85) * scene.x;
    float3 H = normalize(L + V);
    float spec = pow(saturate(dot(n, H)), 48.0) * 0.22 * ndl;
    // hemisphere ambient: sky from above, ground bounce from below, tinted by MHW's frame
    // Fill the deep shadow side enough to retain Remnant surface detail in MHW's high-contrast scenes.
    float3 hemi = lerp(float3(0.64, 0.58, 0.52), float3(0.82, 0.90, 1.0), n.y * 0.5 + 0.5) * scene.y * ambTint.rgb;
    float3 albedo = c.rgb * tint.rgb;
    float3 col = (albedo * (sunCol * ndl * 0.85 + hemi) + sunCol * spec) * scene.z;
    // MHW's frame is already tone-mapped: map the lit colour the same way (ACES, Narkowicz fit)
    col = saturate((col * (2.51 * col + 0.03)) / (col * (2.43 * col + 0.59) + 0.14));
    if (scene.w > 0.5) col = pow(col, 1.0 / 2.2);
    return float4(col, 1);
}
)";

struct GpuModel {
    ID3D11Buffer* vb = nullptr;
    ID3D11Buffer* ib = nullptr;
    bool failed = false;
};

static ID3D11Device* g_dev = nullptr;
static ID3D11VertexShader* g_vs = nullptr;
static ID3D11PixelShader* g_ps = nullptr;
static ID3D11InputLayout* g_layout = nullptr;
static ID3D11Buffer* g_cbFrame = nullptr;
static ID3D11Buffer* g_cbBones = nullptr;
static ID3D11SamplerState* g_samp = nullptr;
static ID3D11RasterizerState* g_rs = nullptr;
static ID3D11DepthStencilState* g_dssLess = nullptr;
static ID3D11DepthStencilState* g_dssGreater = nullptr;
static ID3D11BlendState* g_blend = nullptr;
static ID3D11Texture2D* g_depthTex = nullptr;
static ID3D11DepthStencilView* g_dsv = nullptr;
static ID3D11ShaderResourceView* g_white = nullptr;
static int g_dw = 0, g_dh = 0;
static bool g_failed = false;
static std::map<const Model*, GpuModel> g_models;
static ID3D11ShaderResourceView* g_gameDepth = nullptr;  // MHW's scene depth, when found (render3d_set_game_depth)
static bool g_gameDepthReversed = false;
static int g_gameDepthW = 0, g_gameDepthH = 0;

struct FrameCB { float world[16], viewProj[16], lightDir[4], tint[4], depthInfo[4], camPos[4], scene[4], ambTint[4]; };

SceneLight g_sceneLight;

// Exposure matching: once per second the 8x8 block of the game's frame under the boss is copied (before the boss is
// drawn) into a staging texture and mapped a frame or more later, without waiting for the GPU.
static ID3D11Texture2D* g_stage = nullptr;
static DXGI_FORMAT g_stageFmt = DXGI_FORMAT_UNKNOWN;
static bool g_stagePending = false;
static double g_lastCopy = -10;
static ID3D11Texture2D* g_stageG = nullptr;  // 32x8: four ground blocks (N, E, S, W) side by side
static float g_gpts[4][2];                   // their screen centres at copy time
static bool g_gvalid[4];
static float g_fitVec[2] = {0, 0};           // smoothed brightest-side vector (x, z) of the ground fit
static bool g_fitOk = false;
static float g_sunCap = 1.0f, g_sunCapTarget = 1.0f;  // sun term scale: falls with the sampled ground luminance at night
static float g_curSunY = 0.8f;               // y of the sun in use, for the gain calibration
static float g_expo = 1.0f, g_expoTint[3] = {1, 1, 1};  // smoothed
static float g_medLum = 0.0f;
static float g_expoTarget = 1.0f, g_expoTargetTint[3] = {1, 1, 1};

static float half_to_float(uint16_t h) {
    int e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? 65504.0f : ldexpf((float)(m + 1024), e - 25);
    return (h & 0x8000) ? -v : v;
}

// Average colour (0..1) of an 8x8 block of a mapped texture; false for formats not understood.
static bool average_block(const D3D11_MAPPED_SUBRESOURCE& ms, DXGI_FORMAT fmt, float out[3], int x0 = 0) {
    double sum[3] = {0, 0, 0};
    for (int y = 0; y < 8; y++) {
        const uint8_t* row = (const uint8_t*)ms.pData + (size_t)y * ms.RowPitch;
        for (int x = x0; x < x0 + 8; x++) {
            float c[3];
            switch (fmt) {
                case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                    for (int k = 0; k < 3; k++) c[k] = row[x * 4 + k] / 255.0f;
                    break;
                case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
                    for (int k = 0; k < 3; k++) c[k] = row[x * 4 + 2 - k] / 255.0f;
                    break;
                case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS: {
                    uint32_t v = *(const uint32_t*)(row + x * 4);
                    c[0] = (v & 1023) / 1023.0f; c[1] = ((v >> 10) & 1023) / 1023.0f; c[2] = ((v >> 20) & 1023) / 1023.0f;
                    break;
                }
                case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS:
                    for (int k = 0; k < 3; k++) c[k] = std::min(1.0f, std::max(0.0f, half_to_float(((const uint16_t*)(row + x * 8))[k])));
                    break;
                default: return false;
            }
            for (int k = 0; k < 3; k++) sum[k] += c[k];
        }
    }
    for (int k = 0; k < 3; k++) out[k] = (float)(sum[k] / 64.0);
    return true;
}

static bool g_encodedFmt = true;  // the back buffer holds gamma-encoded values (8/10 bit)
static bool g_outEncode = true;   // ... and the render target view does not encode on write, so the shader must

static float aces(float x) { return std::min(1.0f, std::max(0.0f, (x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f))); }

// [visuals] ground_albedo (default 0.25: real ground is dark, not mid-grey) and exposure_lock (0 = auto), re-read each sample.
static float g_groundAlbedo = 0.25f, g_expoLock = 0.0f;
static void read_visual_ini() {
    char ini[MAX_PATH];
    snprintf(ini, sizeof(ini), "%s\\remnant_hunt.ini", mod_dir());
    auto fv = [&](const char* name, float def) {
        char v[32] = {0};
        GetPrivateProfileStringA("visuals", name, "", v, sizeof(v), ini);
        return v[0] ? (float)atof(v) : def;
    };
    g_groundAlbedo = std::min(0.8f, std::max(0.05f, fv("ground_albedo", 0.25f)));
    g_expoLock = std::max(0.0f, fv("exposure_lock", 0.0f));
}

// Gain that lands a reference ground albedo (ini ground_albedo, 0.25) facing the sun on the sampled ground luminance after
// the same tone curve. Clamped to 0.6..1.8: the readback only nudges exposure, it can never crush it.
static float calibrated_gain(float groundLumEnc) {
    float target = std::min(0.8f, std::max(0.02f, g_encodedFmt ? powf(groundLumEnc, 2.2f) : groundLumEnc));
    float lo = 0, hi = 64;
    for (int i = 0; i < 40; i++) { float m = 0.5f * (lo + hi); (aces(m) < target ? lo : hi) = m; }
    float x = 0.5f * (lo + hi);
    float sunLum = 0.953f * std::max(0.0f, g_sceneLight.sunStrength) * g_sunCapTarget;
    float hemiLum = (0.3856f + (0.793f - 0.3856f) * (g_curSunY * 0.5f + 0.5f)) * std::max(0.0f, g_sceneLight.ambient);
    float lit = g_groundAlbedo * (sunLum * 0.85f + hemiLum);
    return std::min(1.8f, std::max(0.6f, x / std::max(lit, 0.02f)));
}

static ID3D11Texture2D* make_stage(UINT w, DXGI_FORMAT fmt) {
    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = w; sd.Height = 8;
    sd.MipLevels = sd.ArraySize = 1;
    sd.Format = fmt;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* t = nullptr;
    return SUCCEEDED(g_dev->CreateTexture2D(&sd, nullptr, &t)) ? t : nullptr;
}

// Called at the start of the pass with the first boss's screen position and the screen points of the ground 2 m
// N / E / S / W of it; updates the gain target and the ground-fit sun azimuth.
static void sample_scene(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, float px, float py, const float gp[4][2],
                         const bool gv[4], double t) {
    if (g_stagePending) {
        D3D11_MAPPED_SUBRESOURCE ms, mg;
        HRESULT hr = ctx->Map(g_stage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
        if (FAILED(hr)) { g_stagePending = false; return; }
        bool haveG = g_stageG && SUCCEEDED(ctx->Map(g_stageG, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mg));
        if (g_stageG && !haveG) { ctx->Unmap(g_stage, 0); return; }  // the ground copy is still in flight: retry next frame
        g_stagePending = false;
        read_visual_ini();
        // median of the valid ground blocks (N/E/S/W); the boss's own pixel can be body or grass, not lit ground
        float a[3] = {0, 0, 0};
        int nGround = 0;
        if (haveG) {
            float gc[4][3];
            for (int i = 0; i < 4; i++) {
                float c[3];
                if (!g_gvalid[i] || !average_block(mg, g_stageFmt, c, i * 8)) continue;
                float l = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
                if (l < 0.05f || l > 0.98f) continue;  // shadow / sky / UI
                memcpy(gc[nGround++], c, sizeof(c));
            }
            if (nGround >= 2) {
                for (int k = 0; k < 3; k++) {
                    float v[4];
                    for (int i = 0; i < nGround; i++) v[i] = gc[i][k];
                    std::sort(v, v + nGround);
                    a[k] = (nGround & 1) ? v[nGround / 2] : 0.5f * (v[nGround / 2 - 1] + v[nGround / 2]);
                }
            }
        }
        bool haveA = nGround >= 2 || average_block(ms, g_stageFmt, a);
        if (haveA) {
            float lum = 0.2126f * a[0] + 0.7152f * a[1] + 0.0722f * a[2];
            g_medLum = lum;
            bool night = lum < 0.25f;
            g_sunCapTarget = night ? std::min(1.0f, std::max(0.05f, lum / 0.25f)) : 1.0f;
            g_expoTarget = g_expoLock > 0 ? g_expoLock : calibrated_gain(lum);
            for (int k = 0; k < 3; k++) {
                float chroma = a[k] / std::max(lum, 0.02f);  // 1 = grey
                float lo = night ? 0.4f : 0.6f, hi = night ? 1.6f : 1.4f;
                chroma = std::min(hi, std::max(lo, chroma));
                g_expoTargetTint[k] = 1.0f + (night ? 1.0f : 0.7f) * (chroma - 1.0f);  // all of the cast at night, part by day
            }
        }
        {
            static double lastLog = -100;
            if (t - lastLog >= 60.0) {
                lastLog = t;
                log("render3d: readback median lum %.3f (%d ground samples%s) rgb (%.2f,%.2f,%.2f) tint (%.2f,%.2f,%.2f) "
                    "final gain %.2f%s sun cap %.2f ground albedo %.2f%s",
                    g_medLum, nGround, nGround >= 2 ? "" : ", centre block", a[0], a[1], a[2], g_expoTargetTint[0],
                    g_expoTargetTint[1], g_expoTargetTint[2], g_expoTarget, g_expoLock > 0 ? " (locked)" : "", g_sunCapTarget,
                    g_groundAlbedo, g_encodedFmt ? "" : " (float target)");
            }
        }
        ctx->Unmap(g_stage, 0);
        if (haveG) {
            // brightest side = the sun's side (the shadowed ground is darker): contrast-weighted vector sum of N, E, S, W
            static const float dir[4][2] = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}};  // (x, z) of N, E, S, W
            float lum[4], mean = 0;
            int n = 0;
            for (int i = 0; i < 4; i++) {
                float c[3];
                lum[i] = -1;
                if (g_gvalid[i] && average_block(mg, g_stageFmt, c, i * 8)) {
                    lum[i] = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
                    mean += lum[i];
                    n++;
                }
            }
            if (n >= 3) {
                mean /= n;
                float vx = 0, vz = 0;
                for (int i = 0; i < 4; i++)
                    if (lum[i] >= 0) { vx += (lum[i] - mean) * dir[i][0]; vz += (lum[i] - mean) * dir[i][1]; }
                float mag = sqrtf(vx * vx + vz * vz);
                if (mag / (mean + 0.02f) > 0.06f) {  // enough contrast to mean anything
                    vx /= mag; vz /= mag;
                    float k = g_fitOk ? 0.25f : 1.0f;
                    g_fitVec[0] += (vx - g_fitVec[0]) * k;
                    g_fitVec[1] += (vz - g_fitVec[1]) * k;
                    g_fitOk = true;
                }
            }
            ctx->Unmap(g_stageG, 0);
        }
        return;
    }
    if (t - g_lastCopy < 1.0 || !rtv) return;
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    if (!res) return;
    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&bb)) && bb) {
        D3D11_TEXTURE2D_DESC d;
        bb->GetDesc(&d);
        if (d.SampleDesc.Count == 1 && d.Width >= 32 && d.Height >= 8) {
            if (!g_stage || g_stageFmt != d.Format) {
                if (g_stage) { g_stage->Release(); g_stage = nullptr; }
                if (g_stageG) { g_stageG->Release(); g_stageG = nullptr; }
                g_stage = make_stage(8, d.Format);
                g_stageG = make_stage(32, d.Format);
                if (g_stage) g_stageFmt = d.Format;
            }
            if (g_stage) {
                int x = std::min<int>(std::max(0, (int)px - 4), (int)d.Width - 8);
                int y = std::min<int>(std::max(0, (int)py - 4), (int)d.Height - 8);
                D3D11_BOX box = {(UINT)x, (UINT)y, 0, (UINT)x + 8, (UINT)y + 8, 1};
                ctx->CopySubresourceRegion(g_stage, 0, 0, 0, 0, bb, 0, &box);
                for (int i = 0; i < 4; i++) {
                    g_gvalid[i] = gv[i] && g_stageG;
                    if (!g_gvalid[i]) continue;
                    int gx = std::min<int>(std::max(0, (int)gp[i][0] - 4), (int)d.Width - 8);
                    int gy = std::min<int>(std::max(0, (int)gp[i][1] - 4), (int)d.Height - 8);
                    D3D11_BOX gb = {(UINT)gx, (UINT)gy, 0, (UINT)gx + 8, (UINT)gy + 8, 1};
                    ctx->CopySubresourceRegion(g_stageG, 0, (UINT)i * 8, 0, 0, bb, 0, &gb);
                }
                g_stagePending = true;
                g_lastCopy = t;
                bool fl = d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || d.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS;
                g_encodedFmt = !fl;
                DXGI_FORMAT rf = d.Format;
                D3D11_RENDER_TARGET_VIEW_DESC rvd = {};
                rtv->GetDesc(&rvd);
                if (rvd.Format != DXGI_FORMAT_UNKNOWN) rf = rvd.Format;
                bool srgbView = rf == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || rf == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || rf == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
                g_outEncode = g_encodedFmt && !srgbView;
                static DXGI_FORMAT loggedFmt = DXGI_FORMAT_UNKNOWN;
                if (loggedFmt != rf) {
                    loggedFmt = rf;
                    log("render3d: back buffer fmt %d, encode on write %s", (int)rf, g_outEncode ? "yes" : "no");
                }
            }
        }
        bb->Release();
    }
    res->Release();
}

// Which source the sun comes from: ini sun_from (a float3 in a singleton), the ground fit, or the ini angles.
static void pick_sun(double t, float sun[3]) {
    static char lastLabel[96] = "";
    static uintptr_t st = 0;
    static double lastLook = -10;
    static char name[64] = "";
    static size_t off = 0;
    static bool parsed = false;
    char label[96];
    float yaw = g_sceneLight.sunYaw * 0.0174533f, pitch = g_sceneLight.sunPitch * 0.0174533f;
    sun[0] = cosf(pitch) * sinf(yaw); sun[1] = sinf(pitch); sun[2] = cosf(pitch) * cosf(yaw);
    snprintf(label, sizeof(label), "ini");
    bool done = false;
    if (g_sceneLight.sunFrom[0]) {
        if (!parsed) {
            parsed = true;
            const char* plus = strchr(g_sceneLight.sunFrom, '+');
            if (plus) {
                snprintf(name, sizeof(name), "%.*s", (int)std::min<size_t>(sizeof(name) - 1, plus - g_sceneLight.sunFrom), g_sceneLight.sunFrom);
                off = (size_t)strtoul(plus + 1, nullptr, 0);
            } else log("render3d: sun_from=%s needs the form <singleton>+0x<offset>", g_sceneLight.sunFrom);
        }
        if (name[0]) {
            if (!st && t - lastLook > 1.0) { lastLook = t; st = singleton_static(name); }
            uint8_t* o = st ? rd_ptr(st) : nullptr;
            float f[3];
            if (o && mem_readable(o + off, 12)) {
                memcpy(f, o + off, 12);
                float l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
                if (l > 0.9f && l < 1.1f) {
                    float sgn = f[1] < 0 ? -1.0f : 1.0f;  // a direction "from the sun" points down: always light from above
                    for (int k = 0; k < 3; k++) sun[k] = sgn * f[k] / l;
                    snprintf(label, sizeof(label), "singleton %s +0x%zX", name, off);
                    done = true;
                }
            }
        }
    }
    if (!done && g_fitOk) {
        // the ground fit gives the azimuth only; the elevation stays the ini's
        float l = sqrtf(g_fitVec[0] * g_fitVec[0] + g_fitVec[1] * g_fitVec[1]);
        if (l > 0.05f) {
            sun[0] = cosf(pitch) * g_fitVec[0] / l; sun[1] = sinf(pitch); sun[2] = cosf(pitch) * g_fitVec[1] / l;
            snprintf(label, sizeof(label), "ground-fit");
        }
    }
    if (strcmp(label, lastLabel) != 0) {
        log("render3d: sun from %s, dir (%.2f,%.2f,%.2f)", label, sun[0], sun[1], sun[2]);
        snprintf(lastLabel, sizeof(lastLabel), "%s", label);
    }
}

static bool init(ID3D11Device* dev) {
    if (g_vs || g_failed) return !g_failed;
    g_dev = dev;
    ID3DBlob *vsb = nullptr, *psb = nullptr, *errb = nullptr;
    if (FAILED(D3DCompile(kShader, strlen(kShader), "remnant3d", nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vsb, &errb)) ||
        FAILED(D3DCompile(kShader, strlen(kShader), "remnant3d", nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &psb, &errb))) {
        log("render3d: shader compile failed: %s", errb ? (const char*)errb->GetBufferPointer() : "?");
        if (errb) errb->Release();
        if (vsb) vsb->Release();
        if (psb) psb->Release();
        g_failed = true;
        return false;
    }
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R16G16B16A16_SNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 1, DXGI_FORMAT_R16G16B16A16_UINT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 44, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev->CreateInputLayout(il, 7, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_layout);
    vsb->Release();
    psb->Release();
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = sizeof(FrameCB);
    dev->CreateBuffer(&bd, nullptr, &g_cbFrame);
    bd.ByteWidth = 3 * 1024 * 16;
    dev->CreateBuffer(&bd, nullptr, &g_cbBones);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.MaxAnisotropy = 8;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &g_samp);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;  // Remnant II (left-handed) to MHW: winding is not worth trusting
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &g_rs);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    dev->CreateDepthStencilState(&dd, &g_dssLess);
    dd.DepthFunc = D3D11_COMPARISON_GREATER;
    dev->CreateDepthStencilState(&dd, &g_dssGreater);
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bl, &g_blend);
    // 1x1 white texture for materials whose texture is still loading
    uint32_t white = 0xFFB0A090;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 1;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;  // an albedo, like every other diffuse texture: decoded once by the sampler
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sub = {&white, 4, 4};
    ID3D11Texture2D* t = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&td, &sub, &t))) {
        dev->CreateShaderResourceView(t, nullptr, &g_white);
        t->Release();
    }
    if (!(g_vs && g_ps && g_layout && g_cbFrame && g_cbBones && g_samp && g_rs && g_dssLess && g_dssGreater && g_blend)) {
        // Give up for good and free what was made, or every frame would leak a dozen objects.
        log("render3d: creating the pipeline objects failed, bosses will not be drawn");
#define RH_REL(p) if (p) { p->Release(); p = nullptr; }
        RH_REL(g_vs) RH_REL(g_ps) RH_REL(g_layout) RH_REL(g_cbFrame) RH_REL(g_cbBones) RH_REL(g_samp) RH_REL(g_rs)
        RH_REL(g_dssLess) RH_REL(g_dssGreater) RH_REL(g_blend) RH_REL(g_white)
#undef RH_REL
        g_failed = true;
        return false;
    }
    log("render3d: ready");
    return true;
}

static GpuModel* gpu(const Model* m) {
    GpuModel& g = g_models[m];
    if (g.vb || g.failed) return g.failed ? nullptr : &g;
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.ByteWidth = (UINT)(m->vertices.size() * sizeof(ModelVertex));
    D3D11_SUBRESOURCE_DATA sd = {m->vertices.data(), 0, 0};
    if (FAILED(g_dev->CreateBuffer(&bd, &sd, &g.vb))) { g.failed = true; return nullptr; }
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.ByteWidth = (UINT)(m->indices.size() * 4);
    sd.pSysMem = m->indices.data();
    if (FAILED(g_dev->CreateBuffer(&bd, &sd, &g.ib))) { g.failed = true; return nullptr; }
    return &g;
}

static void ensure_depth(int w, int h) {
    if (g_dsv && w == g_dw && h == g_dh) return;
    if (g_dsv) { g_dsv->Release(); g_dsv = nullptr; }
    if (g_depthTex) { g_depthTex->Release(); g_depthTex = nullptr; }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &g_depthTex))) g_dev->CreateDepthStencilView(g_depthTex, nullptr, &g_dsv);
    g_dw = w;
    g_dh = h;
}

void render3d_set_game_depth(void* srv, bool reversed, int width, int height) {
    g_gameDepth = (ID3D11ShaderResourceView*)srv;
    g_gameDepthReversed = reversed;
    g_gameDepthW = width;
    g_gameDepthH = height;
}

// The part of the device context state this pass changes, put back afterwards.
struct Backup {
    ID3D11RasterizerState* rs;
    ID3D11BlendState* bs;
    float bf[4];
    UINT mask;
    ID3D11DepthStencilState* dss;
    UINT sref;
    UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    ID3D11RenderTargetView* rtv[8];
    ID3D11DepthStencilView* dsv;
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11Buffer *vcb[2], *pcb[2];
    ID3D11ShaderResourceView* srv[2];
    ID3D11SamplerState* smp;
    ID3D11InputLayout* il;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11Buffer *vb, *ib;
    UINT stride, off, ibOff;
    DXGI_FORMAT ibFmt;
    void save(ID3D11DeviceContext* c) {
        c->RSGetState(&rs);
        c->OMGetBlendState(&bs, bf, &mask);
        c->OMGetDepthStencilState(&dss, &sref);
        c->RSGetViewports(&nvp, vp);
        c->OMGetRenderTargets(8, rtv, &dsv);
        c->VSGetShader(&vs, nullptr, nullptr);
        c->PSGetShader(&ps, nullptr, nullptr);
        c->VSGetConstantBuffers(0, 2, vcb);
        c->PSGetConstantBuffers(0, 2, pcb);
        c->PSGetShaderResources(0, 2, srv);
        c->PSGetSamplers(0, 1, &smp);
        c->IAGetInputLayout(&il);
        c->IAGetPrimitiveTopology(&topo);
        c->IAGetVertexBuffers(0, 1, &vb, &stride, &off);
        c->IAGetIndexBuffer(&ib, &ibFmt, &ibOff);
    }
    void restore(ID3D11DeviceContext* c) {
        c->RSSetState(rs);
        c->OMSetBlendState(bs, bf, mask);
        c->OMSetDepthStencilState(dss, sref);
        c->RSSetViewports(nvp, vp);
        c->OMSetRenderTargets(8, rtv, dsv);
        c->VSSetShader(vs, nullptr, 0);
        c->PSSetShader(ps, nullptr, 0);
        c->VSSetConstantBuffers(0, 2, vcb);
        c->PSSetConstantBuffers(0, 2, pcb);
        c->PSSetShaderResources(0, 2, srv);
        c->PSSetSamplers(0, 1, &smp);
        c->IASetInputLayout(il);
        c->IASetPrimitiveTopology(topo);
        c->IASetVertexBuffers(0, 1, &vb, &stride, &off);
        c->IASetIndexBuffer(ib, ibFmt, ibOff);
        IUnknown* all[] = {rs, bs, dss, dsv, vs, ps, vcb[0], vcb[1], pcb[0], pcb[1], srv[0], srv[1], smp, il, vb, ib};
        for (IUnknown* u : all)
            if (u) u->Release();
        for (auto* r : rtv)
            if (r) r->Release();
    }
};

void render3d_frame(void* devp, void* ctxp, void* rtvp, int w, int h, const float view[16], const float proj[16],
                    const std::vector<DrawItem>& items) {
    if (items.empty() || w <= 0 || h <= 0) return;
    auto* dev = (ID3D11Device*)devp;
    auto* ctx = (ID3D11DeviceContext*)ctxp;
    auto* rtv = (ID3D11RenderTargetView*)rtvp;
    if (!init(dev)) return;
    ensure_depth(w, h);
    if (!g_dsv) return;
    // Which way MHW's projection maps depth: compare a near and a far point.
    float zn = 0, zf = 0;
    {
        auto ndc = [&](float d) {
            // a point straight ahead in view space; MHW's camera may look down -z or +z: use the one in front
            for (float sign : {-1.0f, 1.0f}) {
                float v[4] = {0, 0, sign * d, 1}, cz = 0, cw = 0;
                for (int i = 0; i < 4; i++) { cz += v[i] * proj[i * 4 + 2]; cw += v[i] * proj[i * 4 + 3]; }
                if (cw > 1e-6f) return cz / cw;
            }
            return 0.0f;
        };
        zn = ndc(100.0f);
        zf = ndc(100000.0f);
    }
    bool reversed = zn > zf;
    float vpm[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += view[i * 4 + k] * proj[k * 4 + j];
            vpm[i * 4 + j] = s;
        }
    {
        // the game's frame under the first boss that is on screen (before anything is drawn into it)
        LARGE_INTEGER fq, now;
        QueryPerformanceFrequency(&fq);
        QueryPerformanceCounter(&now);
        double t = (double)now.QuadPart / (double)fq.QuadPart;
        auto project = [&](float wx, float wy, float wz, float* sx, float* sy) {
            float p[4] = {wx, wy, wz, 1.0f}, cx = 0, cy = 0, cw = 0;
            for (int i = 0; i < 4; i++) {
                cx += p[i] * vpm[i * 4 + 0];
                cy += p[i] * vpm[i * 4 + 1];
                cw += p[i] * vpm[i * 4 + 3];
            }
            if (cw <= 1e-4f) return false;
            *sx = (cx / cw * 0.5f + 0.5f) * w;
            *sy = (0.5f - cy / cw * 0.5f) * h;
            return *sx >= 0 && *sy >= 0 && *sx < w && *sy < h;
        };
        for (const DrawItem& it : items) {
            float sx, sy;
            if (!project(it.world[12], it.world[13], it.world[14], &sx, &sy)) continue;
            // the ground 2 m (200 world units) north (+z), east (+x), south, west of the boss's feet
            static const float off[4][2] = {{0, 200}, {200, 0}, {0, -200}, {-200, 0}};
            float gp[4][2];
            bool gv[4];
            for (int i = 0; i < 4; i++)
                gv[i] = project(it.world[12] + off[i][0], it.world[13], it.world[14] + off[i][1], &gp[i][0], &gp[i][1]);
            sample_scene(ctx, rtv, sx, sy, gp, gv, t);
            break;
        }
        float k = 0.06f;  // eased toward the sampled values, a frame at a time
        if (g_expoLock > 0) g_expo = g_expoTarget = g_expoLock;
        else g_expo += (g_expoTarget - g_expo) * k;
        g_sunCap += (g_sunCapTarget - g_sunCap) * k;
        for (int c = 0; c < 3; c++) g_expoTint[c] += (g_expoTargetTint[c] - g_expoTint[c]) * k;
    }
    Backup bk;
    bk.save(ctx);
    ctx->ClearDepthStencilView(g_dsv, D3D11_CLEAR_DEPTH, reversed ? 0.0f : 1.0f, 0);
    D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &rtv, g_dsv);
    ctx->OMSetDepthStencilState(reversed ? g_dssGreater : g_dssLess, 0);
    float bf[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(g_blend, bf, 0xFFFFFFFF);
    ctx->RSSetState(g_rs);
    ctx->IASetInputLayout(g_layout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ID3D11Buffer* cbs[2] = {g_cbFrame, g_cbBones};
    ctx->VSSetConstantBuffers(0, 2, cbs);
    ctx->PSSetConstantBuffers(0, 2, cbs);
    ctx->PSSetSamplers(0, 1, &g_samp);
    // world-fixed sun (MHW's own vector, the ground fit, or the ini) and the camera position (the view matrix is
    // rigid: camera = -t * R^T)
    float sun[3];
    {
        LARGE_INTEGER fq, now;
        QueryPerformanceFrequency(&fq);
        QueryPerformanceCounter(&now);
        pick_sun((double)now.QuadPart / (double)fq.QuadPart, sun);
        g_curSunY = sun[1];
    }
    float cam[3];
    for (int j = 0; j < 3; j++) cam[j] = -(view[12] * view[j * 4 + 0] + view[13] * view[j * 4 + 1] + view[14] * view[j * 4 + 2]);
    for (const DrawItem& it : items) {
        GpuModel* g = gpu(it.model);
        if (!g || it.palette.size() < it.model->bones.size() * 12) continue;
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(ctx->Map(g_cbFrame, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            FrameCB* f = (FrameCB*)ms.pData;
            memcpy(f->world, it.world, 64);
            memcpy(f->viewProj, vpm, 64);
            f->lightDir[0] = sun[0]; f->lightDir[1] = sun[1]; f->lightDir[2] = sun[2]; f->lightDir[3] = 0;
            f->camPos[0] = cam[0]; f->camPos[1] = cam[1]; f->camPos[2] = cam[2]; f->camPos[3] = 1;
            f->scene[0] = std::max(0.0f, g_sceneLight.sunStrength) * g_sunCap;
            f->scene[1] = std::max(0.0f, g_sceneLight.ambient);
            f->scene[2] = g_expo;
            f->scene[3] = g_outEncode ? 1.0f : 0.0f;
            for (int c = 0; c < 3; c++) f->ambTint[c] = g_expoTint[c];
            f->ambTint[3] = 1;
            memcpy(f->tint, it.tint, 16);
            // MHW's buffer is compared in this pass's own convention: flipped when the two disagree.
            f->depthInfo[0] = g_gameDepth ? (g_gameDepthReversed != reversed ? 2.0f : 1.0f) : 0.0f;
            f->depthInfo[1] = reversed ? 1.0f : 0.0f;
            f->depthInfo[2] = g_gameDepthW > 0 ? (float)g_gameDepthW / (float)w : 1.0f;
            f->depthInfo[3] = g_gameDepthH > 0 ? (float)g_gameDepthH / (float)h : 1.0f;
            ctx->Unmap(g_cbFrame, 0);
        }
        if (SUCCEEDED(ctx->Map(g_cbBones, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            size_t n = std::min<size_t>(it.model->bones.size(), 1024);
            memcpy(ms.pData, it.palette.data(), n * 48);
            ctx->Unmap(g_cbBones, 0);
        }
        UINT stride = sizeof(ModelVertex), off = 0;
        ctx->IASetVertexBuffers(0, 1, &g->vb, &stride, &off);
        ctx->IASetIndexBuffer(g->ib, DXGI_FORMAT_R32_UINT, 0);
        for (const ModelSection& s : it.model->sections) {
            ID3D11ShaderResourceView* srv = g_white;
            if (s.material >= 0 && (size_t)s.material < it.diffuse.size() && it.diffuse[s.material]) {
                int tw = 0, th = 0;
                const int maxSide = std::max(1024, std::min(4096, it.textureMaxSide));
                if (void* t = texture_for(it.diffuse[s.material], maxSide, &tw, &th, true)) srv = (ID3D11ShaderResourceView*)t;
            }
            ID3D11ShaderResourceView* srvs[2] = {srv, g_gameDepth};
            ctx->PSSetShaderResources(0, 2, srvs);
            ctx->DrawIndexed(s.numTriangles * 3, s.baseIndex, 0);
        }
    }
    ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
    ctx->PSSetShaderResources(0, 2, none);
    bk.restore(ctx);
}

}  // namespace rh
