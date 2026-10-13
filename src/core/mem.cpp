// Remnant x MHW mashup core. MIT License (see LICENSE).
#include <cstdlib>

#include "core.h"

namespace rh {

static bool readable_protect(DWORD p) {
    if (p & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                 PAGE_EXECUTE_WRITECOPY)) != 0;
}

bool mem_readable(const void* p, size_t len) {
    uintptr_t a = (uintptr_t)p, end = a + len;
    if (a < 0x10000 || end < a || a > 0x7FFFFFFFFFFFull) return false;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)a, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || !readable_protect(mbi.Protect)) return false;
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return true;
}

// "48 8B ?? 05" style pattern compare at an address.
bool sig_match(uintptr_t at, const char* sig) {
    const uint8_t* p = (const uint8_t*)at;
    size_t i = 0;
    for (const char* s = sig; *s;) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (s[0] == '?') {
            s += (s[1] == '?') ? 2 : 1;
        } else {
            char hex[3] = {s[0], s[1], 0};
            if (!mem_readable(p + i, 1) || p[i] != (uint8_t)strtol(hex, nullptr, 16)) return false;
            s += 2;
        }
        i++;
    }
    return i > 0;
}

}  // namespace rh
