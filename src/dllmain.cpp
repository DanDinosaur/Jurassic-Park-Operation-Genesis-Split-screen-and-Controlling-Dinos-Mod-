#include <windows.h>
#include "mod.h"

// dinput8.dll proxy entry. The real DirectInput exports are provided by
// proxy.cpp (loads the system dinput8 at runtime), so the game's input still works. All we do
// here is kick off the mod bootstrap on load.

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            mod::Start();
            break;
        case DLL_PROCESS_DETACH:
            // reserved != NULL => process is exiting; our thread is already gone and
            // waiting on it under the loader lock would only stall shutdown.
            if (!reserved) mod::Stop();
            break;
    }
    return TRUE;
}
