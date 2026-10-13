// Remnant x MHW mashup. GPL-3.0-or-later (see LICENSE).
#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace rh {

static char g_dir[MAX_PATH] = ".";
static SRWLOCK g_lock = SRWLOCK_INIT;

void set_mod_dir(const char* dir) { snprintf(g_dir, sizeof(g_dir), "%s", dir); }
const char* mod_dir() { return g_dir; }

static void append(const char* file, const char* text) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s", g_dir, file);
    AcquireSRWLockExclusive(&g_lock);
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(h, text, (DWORD)strlen(text), &w, nullptr);
        CloseHandle(h);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

static void vline(const char* file, const char* fmt, va_list ap) {
    char msg[2048];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = snprintf(msg, sizeof(msg), "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    vsnprintf(msg + n, sizeof(msg) - n - 2, fmt, ap);
    strcat(msg, "\r\n");
    append(file, msg);
}

void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vline("remnant_hunt.log", fmt, ap);
    va_end(ap);
}

void melty_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vline("melty.log", fmt, ap);
    va_end(ap);
}

}  // namespace rh

// Called by IM_ASSERT (src/core/rh_imconfig.h). Logs each failing check once.
void rh_imgui_assert(const char* expr, const char* file, int line) {
    static const char* seen[64];
    static int n = 0;
    for (int i = 0; i < n; i++)
        if (seen[i] == expr) return;
    if (n < 64) seen[n++] = expr;
    rh::log("imgui check failed (ignored): %s at %s:%d", expr, file, line);
}
