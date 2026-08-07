// No USB or device enumeration work happens here. HotSync Manager loads this DLL through
// a static import, so DllMain runs under the loader lock.

#include "palmusb.h"

void PortTableInit();
void PortTableShutdown();

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(module);
            PortTableInit();
            LogInit();
            break;

        case DLL_PROCESS_DETACH:
            // On process teardown (reserved != NULL) other threads are already gone and
            // the heap may be unstable, so skip cleanup and let the OS reclaim.
            if (reserved == nullptr) {
                BridgeStop();
                PortTableShutdown();
                LogShutdown();
            }
            break;

        default:
            break;
    }
    return TRUE;
}
