// Port registry. The original kept a singly linked list behind one DLL-global critical
// section (docs/usbport-abi.md, "Thread safety"); a small fixed array is equivalent here
// because HotSync never opens more than one Palm at a time.

#include "palmusb.h"

namespace {

constexpr int kMaxPorts = 8;

CRITICAL_SECTION g_lock;
PalmPort g_ports[kMaxPorts];
bool g_in_use[kMaxPorts];
int g_next_id = 1;

}  // namespace

void PortTableInit() { InitializeCriticalSection(&g_lock); }

void PortTableShutdown() {
    for (int i = 0; i < kMaxPorts; ++i) {
        if (g_in_use[i]) {
            PipeClose(g_ports[i]);
            g_in_use[i] = false;
        }
    }
    DeleteCriticalSection(&g_lock);
}

void PortLock() { EnterCriticalSection(&g_lock); }
void PortUnlock() { LeaveCriticalSection(&g_lock); }

PalmPort* PortFind(int id) {
    if (id == kPalmUsbInvalidPort) return nullptr;
    for (int i = 0; i < kMaxPorts; ++i) {
        if (g_in_use[i] && g_ports[i].id == id) return &g_ports[i];
    }
    return nullptr;
}

PalmPort* PortCreate() {
    for (int i = 0; i < kMaxPorts; ++i) {
        if (g_in_use[i]) continue;
        g_ports[i] = PalmPort{};
        g_ports[i].id = g_next_id++;
        g_in_use[i] = true;
        return &g_ports[i];
    }
    return nullptr;
}

void PortDestroy(int id) {
    for (int i = 0; i < kMaxPorts; ++i) {
        if (!g_in_use[i] || g_ports[i].id != id) continue;
        PipeClose(g_ports[i]);
        g_ports[i] = PalmPort{};
        g_in_use[i] = false;
        return;
    }
}
