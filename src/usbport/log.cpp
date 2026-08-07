// Metadata-only diagnostic log. Payload bytes are never written: a HotSync stream
// carries contacts, calendar entries and memos.

#include "palmusb.h"

#include <cstdarg>
#include <cstdio>
#include <shlobj.h>

namespace {

CRITICAL_SECTION g_log_lock;
HANDLE g_log_file = INVALID_HANDLE_VALUE;
bool g_enabled = false;
bool g_ready = false;

// Bound the file so a stuck poll loop cannot fill the disk.
constexpr DWORD kMaxLogBytes = 4 * 1024 * 1024;

}  // namespace

void LogInit() {
    InitializeCriticalSection(&g_log_lock);
    g_ready = true;

    // Opt-in: set PALMUSB_LOG=1 before starting HotSync Manager.
    char value[8] = {};
    if (GetEnvironmentVariableA("PALMUSB_LOG", value, sizeof(value)) == 0) return;
    if (value[0] != '1') return;

    char dir[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, dir))) return;

    char path[MAX_PATH] = {};
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\RKPalmUSB", dir);
    CreateDirectoryA(path, nullptr);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\RKPalmUSB\\usbport.log", dir);

    g_log_file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_enabled = (g_log_file != INVALID_HANDLE_VALUE);
    LogPrintf("---- log opened, pid %lu ----", GetCurrentProcessId());
}

void LogShutdown() {
    if (!g_ready) return;
    EnterCriticalSection(&g_log_lock);
    if (g_log_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log_file);
        g_log_file = INVALID_HANDLE_VALUE;
    }
    g_enabled = false;
    LeaveCriticalSection(&g_log_lock);
    DeleteCriticalSection(&g_log_lock);
    g_ready = false;
}

void LogPrintf(const char* format, ...) {
    if (!g_enabled || !g_ready) return;

    char message[512];
    va_list args;
    va_start(args, format);
    int body = _vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);
    if (body < 0) return;

    SYSTEMTIME now;
    GetLocalTime(&now);

    char line[640];
    int length = _snprintf_s(line, sizeof(line), _TRUNCATE,
                             "%02u:%02u:%02u.%03u t%-5lu %s\r\n", now.wHour, now.wMinute,
                             now.wSecond, now.wMilliseconds, GetCurrentThreadId(), message);
    if (length <= 0) return;

    EnterCriticalSection(&g_log_lock);
    if (g_log_file != INVALID_HANDLE_VALUE) {
        if (GetFileSize(g_log_file, nullptr) < kMaxLogBytes) {
            DWORD written = 0;
            WriteFile(g_log_file, line, static_cast<DWORD>(length), &written, nullptr);
        } else {
            g_enabled = false;
        }
    }
    LeaveCriticalSection(&g_log_lock);
}
