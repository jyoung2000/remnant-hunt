// Remnant x MHW mashup core: drawing Remnant II's own 3D models into Monster Hunter: World's frame (Direct3D 11).
// GPL-3.0-or-later (see LICENSE).
//
// Each frame, just before the overlay, the creatures and props the mod runs are drawn into the back buffer with
// MHW's own camera (view and projection read from the game). Skinning runs on the GPU from a bone palette the
// animation code fills. The mod keeps its own depth buffer, and where it can find MHW's scene depth it also hides
// what MHW's world puts in front.
#include "render3d.h"

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
    float4 depthInfo;  // x = 1 when MHW's depth is bound, y = reversed z, z/w = depth size / viewport size
};
cbuffer Bones : register(b1) { float4 bones[3 * 1024]; };
Texture2D diffuseTex : register(t0);
Texture2D gameDepth : register(t1);
SamplerState samp : register(s0);
struct VSIn {
    float3 pos : POSITION; float4 nrm : NORMAL; float2 uv : TEXCOORD0;
    uint4 bi0 : BLENDINDICES0; uint4 bi1 : BLENDINDICES1; float4 bw0 : BLENDWEIGHT0; float4 bw1 : BLENDWEIGHT1;
};
struct PSIn { float4 pos : SV_Position; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };
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
    o.nrm = normalize(mul(n, (float3x3)world));
    o.uv = v.uv;
    return o;
}
float4 ps_main(PSIn i) : SV_Target {
    if (depthInfo.x > 0.5) {
        float g = gameDepth.Load(int3(i.pos.xy * depthInfo.zw, 0)).r;
        bool hidden = depthInfo.y > 0.5 ? (g > i.pos.z + 0.00002) : (g < i.pos.z - 0.00002);
        if (hidden) discard;
    }
    float4 c = diffuseTex.Sample(samp, i.uv);
    float ndl = dot(normalize(i.nrm), lightDir.xyz);
    float light = 0.38 + 0.62 * saturate(ndl * 0.5 + 0.5);
    return float4(c.rgb * tint.rgb * light, 1);
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

struct FrameCB { float world[16], viewProj[16], lightDir[4], tint[4], depthInfo[4]; };

static bool init(ID3D11Device* dev) {
    if (g_vs || g_failed) return !g_failed;
    g_dev = dev;
    ID3DBlob *vsb = nullptr, *psb = nullptr, *errb = nullptr;
    if (FAILED(D3DCompile(kShader, strlen(kShader), "remnant3d", nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vsb, &errb)) ||
        FAILED(D3DCompile(kShader, strlen(kShader), "remnant3d", nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &psb, &errb))) {
        log("render3d: shader compile failed: %s", errb ? (const char*)errb->GetBufferPointer() : "?");
        if (errb) errb->Release();
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
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sub = {&white, 4, 4};
    ID3D11Texture2D* t = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&td, &sub, &t))) {
        dev->CreateShaderResourceView(t, nullptr, &g_white);
        t->Release();
    }
    log("render3d: ready");
    return g_vs && g_ps && g_layout && g_cbFrame && g_cbBones;
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
    float vpm[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += view[i * 4 + k] * proj[k * 4 + j];
            vpm[i * 4 + j] = s;
        }
    for (const DrawItem& it : items) {
        GpuModel* g = gpu(it.model);
        if (!g || it.palette.size() < it.model->bones.size() * 12) continue;
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(ctx->Map(g_cbFrame, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            FrameCB* f = (FrameCB*)ms.pData;
            memcpy(f->world, it.world, 64);
            memcpy(f->viewProj, vpm, 64);
            float L[3] = {0.35f, 0.85f, 0.25f};
            float n = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
            f->lightDir[0] = L[0] / n; f->lightDir[1] = L[1] / n; f->lightDir[2] = L[2] / n; f->lightDir[3] = 0;
            memcpy(f->tint, it.tint, 16);
            f->depthInfo[0] = g_gameDepth ? 1.0f : 0.0f;
            f->depthInfo[1] = g_gameDepthReversed ? 1.0f : 0.0f;
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
                if (void* t = texture_for(it.diffuse[s.material], 1024, &tw, &th)) srv = (ID3D11ShaderResourceView*)t;
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
