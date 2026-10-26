#pragma once
// Client process control: launch (port of janus-emulator/tools/launcher), IP redirect,
// hook DLL injection, process listing.
#include "app/config.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <windows.h>

namespace nj {

using LogFn = std::function<void(const std::string&)>;

struct ProcEntry {
    uint32_t    pid = 0;
    std::string exe;
    bool        hookLoaded = false;
};

// Running client processes (Trickster.bin / .exe, TricksterS2.exe, or the configured exe name).
std::vector<ProcEntry> list_client_processes(const Config& cfg);
bool process_has_module(uint32_t pid, const char* moduleName);

// Runs a command line hidden and waits for it.
bool run_hidden(const std::string& cmdline, DWORD timeoutMs, std::string& err);

// CreateProcess with the splash-bypass command line "<ip>,<account>,<group>,Trickster,0,blank<token>".
bool launch_client(const Config& cfg, const std::string& account, PROCESS_INFORMATION& pi, std::string& err);

// Replaces every copy of the live server IP in the client's memory and writes the
// runtime login IP/port globals.
void redirect_client(HANDLE proc, const Config& cfg, const LogFn& log);

// LoadLibraryA(dllPath) in the target (both x86).
bool inject_dll(HANDLE proc, const std::string& dllPath, std::string& err);

} // namespace nj
