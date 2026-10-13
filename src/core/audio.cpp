// Remnant x MHW mashup core: Remnant II's own sounds, played through XAudio2. GPL-3.0-or-later (see LICENSE).
//
// Sounds are Remnant II's Wwise events, decoded from the player's install by r2read (r2_sound). Each event is kept
// in a few variations (Remnant II picks a random take of every layer), played on the default audio device next to
// MHW's own audio. Volume: [audio] volume=0..100 in remnant_hunt.ini.
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <xaudio2.h>

#include "core.h"

namespace rh {

static std::mutex g_lock;
static IXAudio2* g_xa = nullptr;
static IXAudio2MasteringVoice* g_master = nullptr;
static bool g_tried = false;
static float g_volume = 0.8f;
struct Voice { IXAudio2SourceVoice* v; double started; };
static std::vector<Voice> g_voices;
static std::map<std::string, int> g_nextVariant;
static std::map<std::string, double> g_lastPlay;
static constexpr int kVariants = 3, kMaxVoices = 32;

typedef HRESULT(WINAPI* XAudio2Create_t)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);

static bool init_locked() {
    if (g_tried) return g_xa != nullptr;
    g_tried = true;
    char ini[MAX_PATH];
    snprintf(ini, sizeof(ini), "%s\\remnant_hunt.ini", mod_dir());
    g_volume = GetPrivateProfileIntA("audio", "volume", 80, ini) / 100.0f;
    HMODULE dll = LoadLibraryA("xaudio2_9.dll");
    if (!dll) dll = LoadLibraryA("xaudio2_9redist.dll");
    auto create = dll ? (XAudio2Create_t)GetProcAddress(dll, "XAudio2Create") : nullptr;
    if (!create) {
        log("audio: XAudio2 2.9 not available, Remnant sounds are off");
        return false;
    }
    HRESULT hr = create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa) {
        log("audio: XAudio2Create failed (%08lX)", (unsigned long)hr);
        g_xa = nullptr;
        return false;
    }
    hr = g_xa->CreateMasteringVoice(&g_master, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, nullptr, nullptr,
                                    AudioCategory_GameEffects);
    if (FAILED(hr)) {
        log("audio: no mastering voice (%08lX)", (unsigned long)hr);
        g_xa->Release();
        g_xa = nullptr;
        return false;
    }
    log("audio: XAudio2 ready, volume %.0f%%", g_volume * 100.0f);
    return true;
}

static void reap_locked(bool force) {
    for (size_t i = 0; i < g_voices.size();) {
        XAUDIO2_VOICE_STATE st;
        g_voices[i].v->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (st.BuffersQueued == 0 || (force && i == 0)) {
            g_voices[i].v->DestroyVoice();
            g_voices.erase(g_voices.begin() + i);
            force = false;
        } else {
            i++;
        }
    }
}

void audio_prefetch(const char* event) {
    if (!event || !*event) return;
    for (int v = 0; v < kVariants; v++) r2_sound(event, v);
}

void audio_play(const char* event, float volume) {
    if (!event || !*event || volume <= 0) return;
    std::lock_guard<std::mutex> l(g_lock);
    if (!init_locked() || g_volume <= 0) return;
    std::string key(event);
    double t = GetTickCount64() / 1000.0;
    double& last = g_lastPlay[key];
    if (t - last < 0.045) return;  // very fast guns: one sound per frame or two is plenty
    int& next = g_nextVariant[key];
    const R2Sound* s = r2_sound(event, next % kVariants);
    if (!s) {
        audio_prefetch(event);  // decoding; the next shot will have it
        return;
    }
    next++;
    if (s->failed || s->pcm.empty() || s->rate <= 0) return;
    reap_locked(false);
    if ((int)g_voices.size() >= kMaxVoices) reap_locked(true);
    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = (DWORD)s->rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * 4;
    IXAudio2SourceVoice* v = nullptr;
    if (FAILED(g_xa->CreateSourceVoice(&v, &wf, 0, 2.0f, nullptr, nullptr, nullptr)) || !v) return;
    XAUDIO2_BUFFER b = {};
    b.AudioBytes = (UINT32)(s->pcm.size() * 2);
    b.pAudioData = (const BYTE*)s->pcm.data();  // the cache never drops a decoded sound, so this stays valid
    b.Flags = XAUDIO2_END_OF_STREAM;
    v->SetVolume(volume * g_volume);
    if (FAILED(v->SubmitSourceBuffer(&b, nullptr)) || FAILED(v->Start(0, 0))) {
        v->DestroyVoice();
        return;
    }
    g_voices.push_back({v, t});
    last = t;
}

}  // namespace rh
