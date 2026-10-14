// Host test of the icon pipeline the screens use: the real r2client starts the real r2read.exe against the player's
// Remnant II install, fetches a broad sample of the sheets' icons and art, and texture.cpp uploads them to a real
// Direct3D 11 device, exactly as overlay.cpp does in the game. Needs Remnant II installed; no game running.
// usage: test_textures.exe <directory holding r2read.exe>
#include <d3d11.h>
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "state.h"

namespace rh {
static char g_dir[MAX_PATH];
const char* mod_dir() { return g_dir; }
void log(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    printf("\n");
}
double now_s() { return GetTickCount64() / 1000.0; }
bool r2_make_texture(ID3D11Device* dev, const R2Image& img, const char* asset, ID3D11ShaderResourceView** srv);
}  // namespace rh
using namespace rh;

int main(int argc, char** argv) {
    snprintf(g_dir, sizeof(g_dir), "%s", argc > 1 ? argv[1] : ".");
    if (!r2_start()) { printf("textures: r2read did not start (is Remnant II installed?)\n"); return 2; }
    for (int i = 0; i < 600 && !r2_ready(); i++) Sleep(100);
    if (!r2_ready()) { printf("textures: r2read not ready after 60 s\n"); return 2; }

    // What the screens draw: every icon kind, a wide sample of each, at the sizes the screens ask for.
    struct Want { const char* asset; int side; const char* kind; };
    std::vector<Want> want;
    auto add = [&](const char* a, int side, const char* kind) { if (a && *a) want.push_back({a, side, kind}); };
    for (int i = 0; i < kArchetypesCount; i++) add(kArchetypes[i].emblem, 256, "archetype");
    for (int i = 0; i < kSkillsCount; i++) add(kSkills[i].icon, 128, "skill");
    for (int i = 0; i < kTraitsCount; i++) add(kTraits[i].icon, 128, "trait");
    for (int i = 0; i < kWeaponsCount; i += 2) add(kWeapons[i].icon, 128, "weapon");
    for (int i = 0; i < kWeaponsCount; i += 7) add(kWeapons[i].icon, 512, "weapon-detail");
    for (int i = 0; i < kModsCount; i++) add(kMods[i].icon, 128, "mod");
    for (int i = 0; i < kMutatorsCount; i += 3) add(kMutators[i].icon, 128, "mutator");
    for (int i = 0; i < kRingsCount; i += 8) add(kRings[i].icon, 128, "ring");
    for (int i = 0; i < kAmuletsCount; i += 4) add(kAmulets[i].icon, 128, "amulet");
    for (int i = 0; i < kRelicsCount; i++) add(kRelics[i].icon, 128, "relic");
    for (int i = 0; i < kFragmentsCount; i += 2) add(kFragments[i].icon, 128, "fragment");
    for (int i = 0; i < kConsumablesCount; i += 2) add(kConsumables[i].icon, 128, "consumable");
    for (int i = 0; i < kArmorCount; i += 5) add(kArmor[i].icon, 128, "armor");
    for (int i = 0; i < kDifficultiesCount; i++) add(kDifficulties[i].icon, 128, "difficulty");
    for (int i = 0; i < kUiCount; i++) add(kUi[i].art, 1024, "ui-art");
    printf("textures: asking for %zu images\n", want.size());

    // Queue everything, then wait for the worker (the screens poll the same way, one frame at a time).
    std::vector<const R2Image*> got(want.size(), nullptr);
    size_t done = 0;
    for (int tick = 0; tick < 1800 && done < want.size(); tick++) {
        done = 0;
        for (size_t i = 0; i < want.size(); i++) {
            if (!got[i]) got[i] = r2_image(want[i].asset, want[i].side);
            if (got[i]) done++;
        }
        if (done < want.size()) Sleep(100);
    }
    printf("textures: %zu of %zu images read\n", done, want.size());

    ID3D11Device* dev = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, nullptr);
    if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, nullptr);
    if (FAILED(hr)) { printf("textures: no Direct3D 11 device (0x%08lx)\n", (unsigned long)hr); return 2; }

    std::map<std::string, int> okByKind, badByKind, formats;
    int ok = 0, bad = 0;
    for (size_t i = 0; i < want.size(); i++) {
        const R2Image* img = got[i];
        ID3D11ShaderResourceView* srv = nullptr;
        bool good = img && !img->failed && r2_make_texture(dev, *img, want[i].asset, &srv);
        if (good) { ok++; okByKind[want[i].kind]++; formats[img->format]++; srv->Release(); }
        else { bad++; badByKind[want[i].kind]++; printf("FAIL %s %s%s\n", want[i].kind, want[i].asset, img ? (img->failed ? " (r2read failed)" : "") : " (never read)"); }
    }
    for (auto& k : okByKind) printf("  %-14s ok %4d  fail %3d\n", k.first.c_str(), k.second, badByKind[k.first]);
    for (auto& f : formats) printf("  format %-6s %d\n", f.first.c_str(), f.second);
    dev->Release();
    r2_stop();
    printf("textures: %d of %zu icons uploaded to Direct3D 11%s\n", ok, want.size(), bad ? "" : ", all passed");
    return bad ? 1 : 0;
}
