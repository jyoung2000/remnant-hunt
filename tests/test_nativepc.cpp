// Host test of the nativePC writers: the original item_eng.gmd and itemData.itm of this install (extracted by mhwread into
// build/mhw/) are parsed and written back byte for byte, then extended and re-parsed. Needs no game, only those two files.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core.h"

namespace rh {
const char* mod_dir() { return "."; }
void log(const char*, ...) {}
bool game_api_ready() { return false; }
void* get_resource(const char*, const char*) { return nullptr; }
const char* gmd_message(void*, int) { return nullptr; }
bool r2_ready() { return false; }
const std::string& r2_text(const sheets::R2Text&) { static const std::string empty; return empty; }
}  // namespace rh
using namespace rh;

static int g_fail = 0;
#define CHECK(c)                                                                   \
    do {                                                                           \
        if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } \
    } while (0)

static std::vector<uint8_t> slurp(const char* p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "build/mhw";
    std::vector<uint8_t> gmd = slurp((dir + "/item_eng.gmd").c_str()), itm = slurp((dir + "/itemData.itm").c_str());
    CHECK(!gmd.empty() && !itm.empty());

    NpGmd g;
    CHECK(np_gmd_parse(gmd.data(), gmd.size(), &g));
    CHECK(g.msgs.size() == 5550 && g.msgs[10] == "Antidote");  // item id 5 -> message 10
    CHECK(np_gmd_write(g) == gmd);  // the original, written back, byte for byte

    std::vector<uint8_t> t = itm;
    CHECK(np_itm_add(&t, 0, 0) && t == itm);  // adding nothing changes nothing
    CHECK(np_itm_add(&t, 5, 9));
    CHECK(t.size() == itm.size() + 9 * 32 && t[6] == (2775 + 9) % 256 && t[7] == (2775 + 9) / 256);
    CHECK(memcmp(t.data() + 10, itm.data() + 10, 2775 * 32) == 0);  // old rows untouched
    for (int i = 0; i < 9; i++) {
        const uint8_t* r = t.data() + 10 + (2775 + i) * 32;
        CHECK(r[0] + r[1] * 256 == 2775 + i);
        CHECK(memcmp(r + 2, itm.data() + 10 + 5 * 32 + 2, 30) == 0);  // everything else is the Antidote's
    }
    CHECK(np_itm_add(&t, 205, 3));  // boss loot is inert material, never a drinkable action item
    CHECK(t[6] + t[7] * 256 == 2775 + 9 + 3);
    for (int i = 0; i < 3; i++) {
        const uint8_t* r = t.data() + 10 + (2775 + 9 + i) * 32;
        CHECK(r[0] + r[1] * 256 == 2775 + 9 + i);
        CHECK(memcmp(r + 2, itm.data() + 10 + 205 * 32 + 2, 30) == 0);
    }

    NpGmd g2 = g;
    for (int i = 0; i < 9; i++) {
        np_gmd_add(&g2, "REMNANT_X" + std::to_string(i) + "_NAME", "Name " + std::to_string(i));
        np_gmd_add(&g2, "REMNANT_X" + std::to_string(i) + "_EXP", "Desc " + std::to_string(i));
    }
    std::vector<uint8_t> out = np_gmd_write(g2);
    NpGmd back;
    CHECK(np_gmd_parse(out.data(), out.size(), &back));
    CHECK(back.msgs.size() == 5568 && back.msgs[2775 * 2] == "Name 0" && back.msgs[5567] == "Desc 8");
    CHECK(back.msgs[10] == g.msgs[10] && back.labels == g2.labels);
    CHECK(np_gmd_write(back) == out);

    if (g_fail == 0) printf("nativepc: round trip ok\n");
    return g_fail ? 1 : 0;
}
