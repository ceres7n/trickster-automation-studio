#include "app/client_proc.h"
#include "app/text_util.h"
#include <tlhelp32.h>

namespace nj {

namespace {

// The older KTO build (c79d7a2c), confirmed in IDA (sub_A1CA40 / sub_6A1050): the runtime
// login IP string (20 bytes) and port (u32) globals. Not writable in Client_live.
const uintptr_t kAddrLoginIp   = 0x132E584;
const uintptr_t kAddrLoginPort = 0x132E604;

std::string file_name(const std::string& path)
{
    size_t s = path.find_last_of("\\/");
    return s == std::string::npos ? path : path.substr(s + 1);
}

// The name without its extension ("Trickster.bin" -> "Trickster").
std::string file_stem(const std::string& name)
{
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

// The client ships as Trickster.bin, but a copy may be renamed Trickster.exe (or the
// configured path may use either); the Season 2 client is TricksterS2.exe. Match by base
// name, ignoring the .bin / .exe extension. (The hook finds out which build it is in.)
bool is_client_exe(const std::string& exeName, const std::string& configuredName)
{
    const std::string stem = file_stem(exeName);
    if (_stricmp(stem.c_str(), "Trickster") == 0 || _stricmp(stem.c_str(), "TricksterS2") == 0) return true;
    if (configuredName.empty()) return false;
    if (_stricmp(exeName.c_str(), configuredName.c_str()) == 0) return true;   // exact (any extension)
    return _stricmp(stem.c_str(), file_stem(configuredName).c_str()) == 0;     // same stem, either extension
}

} // namespace

std::vector<ProcEntry> list_client_processes(const Config& cfg)
{
    std::vector<ProcEntry> out;
    const std::string want = file_name(cfg.tricksterPath);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32 pe{ sizeof(pe) };
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe)) {
        std::string exe = pe.szExeFile;
        if (!is_client_exe(exe, want)) continue;
        out.push_back({ pe.th32ProcessID, exe, process_has_module(pe.th32ProcessID, "tas_hook.dll") });
    }
    CloseHandle(snap);
    return out;
}

bool process_has_module(uint32_t pid, const char* moduleName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32 me{ sizeof(me) };
    bool found = false;
    for (BOOL ok = Module32First(snap, &me); ok && !found; ok = Module32Next(snap, &me))
        found = _stricmp(me.szModule, moduleName) == 0;
    CloseHandle(snap);
    return found;
}

bool run_hidden(const std::string& cmdline, DWORD timeoutMs, std::string& err)
{
    STARTUPINFOA si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<char> cmd(cmdline.begin(), cmdline.end());
    cmd.push_back('\0');
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        err = fmt("CreateProcess failed (%lu)", GetLastError());
        return false;
    }
    bool ok = WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0;
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (!ok) { err = "timed out"; return false; }
    if (code != 0) { err = fmt("exit code %lu", code); return false; }
    return true;
}

bool launch_client(const Config& cfg, const std::string& account, PROCESS_INFORMATION& pi, std::string& err)
{
    // Accept the client as Trickster.bin or Trickster.exe: if the configured path is not
    // there, try the same name with the other extension.
    std::string path = cfg.tricksterPath;
    if (!file_exists(path)) {
        size_t dot = path.find_last_of('.');
        if (dot != std::string::npos) {
            std::string ext = path.substr(dot);
            std::string alt = path.substr(0, dot) + (_stricmp(ext.c_str(), ".bin") == 0 ? ".exe" : ".bin");
            if (file_exists(alt)) path = alt;
        }
    }
    if (!file_exists(path)) {
        err = "client not found: " + cfg.tricksterPath;
        return false;
    }
    // Same command line splash.exe / janus_launcher builds.
    std::string args = fmt("\"%s\" \"%s,%s,%s,Trickster,0,blank%s\"", path.c_str(),
                           cfg.loginIp.c_str(), account.c_str(), cfg.groupId.c_str(), cfg.token.c_str());
    std::string cwd = path.substr(0, path.find_last_of("\\/"));

    STARTUPINFOA si{ sizeof(si) };
    std::vector<char> cmd(args.begin(), args.end());
    cmd.push_back('\0');
    if (!CreateProcessA(path.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        cwd.c_str(), &si, &pi)) {
        err = fmt("CreateProcess failed (%lu)", GetLastError());
        return false;
    }
    return true;
}

void redirect_client(HANDLE proc, const Config& cfg, const LogFn& log)
{
    const std::string& from = cfg.originalIp;
    const std::string& to   = cfg.loginIp;
    if (!from.empty() && to.size() <= from.size()) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        char* addr = (char*)si.lpMinimumApplicationAddress;
        char* end  = (char*)si.lpMaximumApplicationAddress;
        std::vector<char> buf(1 << 16);
        int patches = 0;
        MEMORY_BASIC_INFORMATION mi;
        while (addr < end && VirtualQueryEx(proc, addr, &mi, sizeof(mi))) {
            const DWORD prot = mi.Protect & 0xFF;
            const bool readable = prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE ||
                                  prot == PAGE_READONLY || prot == PAGE_EXECUTE_READ;
            if (mi.State == MEM_COMMIT && readable && !(mi.Protect & PAGE_GUARD)) {
                char* p = (char*)mi.BaseAddress;
                char* regionEnd = p + mi.RegionSize;
                while (p < regionEnd) {
                    SIZE_T want = (SIZE_T)(regionEnd - p) < buf.size() ? (SIZE_T)(regionEnd - p) : buf.size();
                    SIZE_T got = 0;
                    if (!ReadProcessMemory(proc, p, buf.data(), want, &got) || got < from.size()) break;
                    for (SIZE_T i = 0; i + from.size() <= got; ++i) {
                        if (memcmp(buf.data() + i, from.data(), from.size()) != 0) continue;
                        char rep[64] = {};
                        memcpy(rep, to.data(), to.size());   // the rest stays NUL
                        DWORD old;
                        VirtualProtectEx(proc, p + i, from.size(), PAGE_READWRITE, &old);
                        WriteProcessMemory(proc, p + i, rep, from.size(), nullptr);
                        VirtualProtectEx(proc, p + i, from.size(), old, &old);
                        ++patches;
                    }
                    // overlap so a string across two reads is not missed
                    p += got > from.size() ? got - from.size() + 1 : got;
                }
            }
            addr = (char*)mi.BaseAddress + mi.RegionSize;
        }
        log(fmt("IP %s -> %s: %d copies patched", from.c_str(), to.c_str(), patches));
    }

    if (!cfg.patchLoginGlobals) return;
    char ip[20] = {};
    strncpy(ip, to.c_str(), sizeof(ip) - 1);
    DWORD port = (DWORD)cfg.loginPort;
    bool okIp   = WriteProcessMemory(proc, (LPVOID)kAddrLoginIp, ip, sizeof(ip), nullptr) != FALSE;
    bool okPort = WriteProcessMemory(proc, (LPVOID)kAddrLoginPort, &port, sizeof(port), nullptr) != FALSE;
    log(fmt("login globals: ip %s, port %u%s", ip, (unsigned)port, okIp && okPort ? "" : " (write FAILED)"));
}

bool inject_dll(HANDLE proc, const std::string& dllPath, std::string& err)
{
    if (!file_exists(dllPath)) {
        err = "hook DLL not found: " + dllPath;
        return false;
    }
    SIZE_T n = dllPath.size() + 1;
    LPVOID remote = VirtualAllocEx(proc, nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { err = fmt("VirtualAllocEx failed (%lu)", GetLastError()); return false; }
    WriteProcessMemory(proc, remote, dllPath.c_str(), n, nullptr);

    // kernel32 sits at the same address in every process of a session (same bitness).
    auto loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE t = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
    if (!t) {
        err = fmt("CreateRemoteThread failed (%lu)", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return false;
    }
    bool done = WaitForSingleObject(t, 10000) == WAIT_OBJECT_0;
    DWORD mod = 0;
    GetExitCodeThread(t, &mod);
    CloseHandle(t);
    if (!done) { err = "LoadLibrary did not return in 10 s"; return false; }
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    if (!mod) { err = "LoadLibrary failed inside the client"; return false; }
    return true;
}

} // namespace nj
