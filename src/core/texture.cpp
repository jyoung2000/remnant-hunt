// Remnant x MHW mashup core: a Remnant II image (BC7/BC3/BC5/BC1/BC4/BGRA8 from r2read) becomes a Direct3D 11 shader
// resource. GPL-3.0-or-later (see LICENSE). Shared by the overlay and tests/test_textures.cpp, so the host test runs
// the same upload the game does.
#include <d3d11.h>

#include <string>

#include "core.h"

namespace rh {

static DXGI_FORMAT dxgi_format(const std::string& f, int* blockBytes, bool srgb) {
    *blockBytes = 16;
    if (f == "BC7") return srgb ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
    if (f == "BC3") return srgb ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM;
    if (f == "BC5") return DXGI_FORMAT_BC5_UNORM;
    *blockBytes = 8;
    if (f == "BC1") return srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
    if (f == "BC4") return DXGI_FORMAT_BC4_UNORM;
    *blockBytes = 0;
    if (f == "BGRA8") return srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
    return DXGI_FORMAT_UNKNOWN;
}

bool r2_make_texture(ID3D11Device* dev, const R2Image& img, const char* asset, ID3D11ShaderResourceView** srv) {
    return r2_make_texture(dev, img, asset, srv, false);
}

// srgb = true for albedo / base-colour textures (the sampler then returns linear); normal and mask maps stay false.
bool r2_make_texture(ID3D11Device* dev, const R2Image& img, const char* asset, ID3D11ShaderResourceView** srv, bool srgb) {
    *srv = nullptr;
    int bb = 0;
    DXGI_FORMAT fmt = dxgi_format(img.format, &bb, srgb);
    size_t want = bb ? (size_t)((img.w + 3) / 4) * (size_t)((img.h + 3) / 4) * bb : (size_t)img.w * img.h * 4;
    if (img.failed || fmt == DXGI_FORMAT_UNKNOWN || img.w <= 0 || img.h <= 0 || img.pixels.size() < want) {
        if (!img.failed) log("overlay: image %s (%s %dx%d) has %zu bytes, %zu needed", asset, img.format.c_str(), img.w, img.h, img.pixels.size(), want);
        return false;
    }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = img.w;
    d.Height = img.h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = img.pixels.data();
    sd.SysMemPitch = bb ? ((img.w + 3) / 4) * bb : img.w * 4;
    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = dev->CreateTexture2D(&d, &sd, &tex);
    if (FAILED(hr)) {
        log("overlay: texture %s (%s %dx%d) could not be created (0x%08lx)", asset, img.format.c_str(), img.w, img.h, (unsigned long)hr);
        return false;
    }
    hr = dev->CreateShaderResourceView(tex, nullptr, srv);
    tex->Release();
    if (FAILED(hr)) {
        log("overlay: shader view for %s could not be created (0x%08lx)", asset, (unsigned long)hr);
        return false;
    }
    return true;
}

}  // namespace rh
