// tas_hook.dll — injected into Trickster.bin by TricksterAutomationStudio.exe.
#include "hook/hooks.h"
#include "hook/ipc_server.h"
#include "hook/packet_store.h"

namespace {

constexpr DWORD kUnpackTimeoutMs = 120000;

DWORD WINAPI init_thread(LPVOID)
{
    // Pin: the hooks jump into this module, it must never be unloaded.
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       (LPCSTR)&init_thread, &self);

    nj::dll_log("tas_hook loaded (pid %u, client base 0x%08X)",
                (unsigned)GetCurrentProcessId(), (unsigned)(uintptr_t)GetModuleHandleA(nullptr));
    nj::ipc_start();
    nj::hooks_install(kUnpackTimeoutMs);
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE t = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
