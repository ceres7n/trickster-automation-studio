// Inline capture hooks on the client's two plaintext choke points.
//
// Each site gets a 5-byte jmp to a naked detour that saves every register, hands a
// pointer to them to a C++ callback, restores them and jumps to a trampoline holding
// the original prologue (position-independent) followed by a jmp back. The callback
// copies the payload into the PacketStore; it never changes the packet.
#include "hook/hooks.h"
#include "hook/client_addr.h"
#include "hook/packet_store.h"
#include <cstring>
#include <string>

namespace nj {
thread_local bool t_injecting = false;
}

using namespace nj;

// Layout on the stack after `pushad; pushfd` at function entry.
struct HookRegs {
    uint32_t eflags, edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t ret;       // caller's return address
    uint32_t args[8];   // stack args (both hooked functions: op, param, src, size, ...)
};

static uint8_t* s_trampC2S = nullptr;
static uint8_t* s_trampS2C = nullptr;
static volatile uint32_t s_state = HOOK_WAITING;

static uint32_t safe_copy(void* dst, const void* src, uint32_t n)
{
    __try {
        memcpy(dst, src, n);
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static void capture(uint8_t dir, const HookRegs* r)
{
    // C2S c2s_frame_packet  __stdcall(opcode, param, Src, Size): opcode = args[0] (both clients).
    // S2C, Paula post_game_event  __thiscall(this, a2, opcode, Src, Size): there is a leading
    //   arg (a2, a category that is 0 or 2) BEFORE the opcode, so opcode = args[1].
    //   Payload and size stay at args[2]/args[3] (verified live 2026-10-20: S2C 11502 came in
    //   as args[0]=0, args[1]=0x2CEE, args[2]=valid 88-byte body).
    // S2C, Season 2 NetworkManager::OnPacket  __thiscall(this, category, param, opcode, cursor):
    //   cursor = {u32 data, u32 size, u32 limit, u32 offset}.
    // The S2C param field carries the category (the connection) for both.
    const bool     c2s    = dir == DIR_C2S;
    const auto*    L      = client::active();
    const bool     cursor = !c2s && L && L->s2cArgs == client::S2CArgs::Cursor;
    uint16_t       op     = (uint16_t)r->args[c2s ? 0 : 1];
    const uint16_t param  = (uint16_t)r->args[c2s ? 1 : 0];
    const void*    src    = (const void*)(uintptr_t)r->args[2];
    uint32_t       size   = r->args[3];
    if (cursor) {
        op = (uint16_t)r->args[2];
        uint32_t cur[2] = {};
        if (!r->args[3] || safe_copy(cur, (const void*)(uintptr_t)r->args[3], sizeof(cur)) != sizeof(cur)) cur[0] = cur[1] = 0;
        src = (const void*)(uintptr_t)cur[0];
        size = cur[0] ? cur[1] : 0;
    }

    uint8_t flags = (dir == DIR_C2S && t_injecting) ? PF_INJECTED : 0;
    uint32_t keep = src ? (size < PacketStore::kMaxKeep ? size : PacketStore::kMaxKeep) : 0;
    std::vector<uint8_t> data(keep);
    if (keep && safe_copy(data.data(), src, keep) != keep) data.clear();
    if (data.size() < size) flags |= PF_TRUNCATED;

    store().push_packet(dir, flags, op, param, r->ecx, size, std::move(data));
}

// C2S: ecx = connection + Layout::ctxToConn (set by Net_SendPacket_Raw).
static void __cdecl on_c2s(HookRegs* r)
{
    try { capture(DIR_C2S, r); } catch (...) {}
}

// S2C: ecx = this of post_game_event / NetworkManager::OnPacket.
static void __cdecl on_s2c(HookRegs* r)
{
    try { capture(DIR_S2C, r); } catch (...) {}
}

static __declspec(naked) void detour_c2s()
{
    __asm {
        pushad
        pushfd
        push esp
        call on_c2s
        add esp, 4
        popfd
        popad
        jmp dword ptr [s_trampC2S]
    }
}

static __declspec(naked) void detour_s2c()
{
    __asm {
        pushad
        pushfd
        push esp
        call on_s2c
        add esp, 4
        popfd
        popad
        jmp dword ptr [s_trampS2C]
    }
}

namespace {

struct Site {
    const char*    name;
    uint32_t       rva;
    const uint8_t* stub;
    uint32_t       stubLen;   // 5..8
    void*          detour;
    uint8_t**      tramp;
};

std::string hex_bytes(const uint8_t* b, size_t n)
{
    std::string s;
    char t[4];
    for (size_t i = 0; i < n; ++i) { snprintf(t, sizeof(t), "%02X ", b[i]); s += t; }
    if (!s.empty()) s.pop_back();
    return s;
}

enum class SiteCheck { Match, Foreign, Other, Unreadable };

SiteCheck check_site(uintptr_t addr, const Site& s, uint8_t* cur8)
{
    if (safe_copy(cur8, (const void*)addr, 8) != 8) return SiteCheck::Unreadable;
    if (memcmp(cur8, s.stub, s.stubLen) == 0) return SiteCheck::Match;
    if (cur8[0] == 0xE9) return SiteCheck::Foreign;   // a jmp someone else left there
    return SiteCheck::Other;
}

bool install_site(uintptr_t addr, const Site& s)
{
    // Trampoline: original prologue + jmp back to the first untouched instruction.
    auto* t = (uint8_t*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return false;
    memcpy(t, s.stub, s.stubLen);
    t[s.stubLen] = 0xE9;
    *(int32_t*)(t + s.stubLen + 1) = (int32_t)((addr + s.stubLen) - ((uintptr_t)t + s.stubLen + 5));
    FlushInstructionCache(GetCurrentProcess(), t, 32);
    *s.tramp = t;   // live before the jmp is

    DWORD old = 0;
    if (!VirtualProtect((void*)addr, 8, PAGE_EXECUTE_READWRITE, &old)) return false;

    // jmp detour (+ nop over the rest of the prologue), written as one atomic 8-byte
    // exchange so a client thread never sees half an instruction.
    LONG64 cur = *(volatile LONG64*)addr;
    uint8_t patch[8];
    memcpy(patch, &cur, 8);
    patch[0] = 0xE9;
    *(int32_t*)(patch + 1) = (int32_t)((uintptr_t)s.detour - (addr + 5));
    for (uint32_t i = 5; i < s.stubLen; ++i) patch[i] = 0x90;
    LONG64 want;
    memcpy(&want, patch, 8);
    InterlockedCompareExchange64((volatile LONG64*)addr, want, cur);

    VirtualProtect((void*)addr, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, 8);
    return memcmp((const void*)addr, patch, 8) == 0;
}

void set_state(uint32_t st)
{
    s_state = st;
    EvHookState e{ st };
    store().post_event(EV_HOOK_STATE, &e, sizeof(e));
}

} // namespace

namespace nj {

uint32_t hooks_state() { return s_state; }

uint32_t hooks_install(uint32_t timeoutMs)
{
    const uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    auto sites_of = [](const client::Layout& L, Site out[2]) {
        out[0] = { "C2S framer", L.rvaC2SFrame, L.c2sStub, L.c2sStubLen, (void*)&detour_c2s, &s_trampC2S };
        out[1] = { L.s2cArgs == client::S2CArgs::Cursor ? "S2C NetworkManager::OnPacket" : "S2C post_game_event",
                   L.rvaS2CEvent, L.s2cStub, L.s2cStubLen, (void*)&detour_s2c, &s_trampS2C };
    };

    // The client build: the layout whose two sites both hold their prologues (the code may still be
    // unpacking for a while, so every layout is tried again until one matches or the time is up).
    const DWORD start = GetTickCount();
    bool announced = false;
    const client::Layout* L = nullptr;
    for (;;) {
        for (size_t k = 0; k < client::kLayoutCount && !L; ++k) {
            Site s[2];
            sites_of(client::kLayouts[k], s);
            uint8_t cur[2][8];
            const SiteCheck c0 = check_site(base + s[0].rva, s[0], cur[0]);
            const SiteCheck c1 = check_site(base + s[1].rva, s[1], cur[1]);
            if (c0 == SiteCheck::Match && c1 == SiteCheck::Match) { L = &client::kLayouts[k]; break; }
            // one site is this build's and the other holds a jmp: someone else hooked it
            if ((c0 == SiteCheck::Match && c1 == SiteCheck::Foreign) || (c0 == SiteCheck::Foreign && c1 == SiteCheck::Match)) {
                const int i = c0 == SiteCheck::Foreign ? 0 : 1;
                dll_log("%s: %s @ 0x%08X already holds a jmp (%s): another tool (janus?) hooked it. Not hooking.",
                        client::kLayouts[k].name, s[i].name, (unsigned)(base + s[i].rva), hex_bytes(cur[i], 8).c_str());
                set_state(HOOK_FAILED);
                return HOOK_FAILED;
            }
        }
        if (L) break;
        if (GetTickCount() - start >= timeoutMs) {
            for (size_t k = 0; k < client::kLayoutCount; ++k) {
                Site s[2];
                sites_of(client::kLayouts[k], s);
                for (const Site& x : s) {
                    uint8_t cur[8];
                    const SiteCheck c = check_site(base + x.rva, x, cur);
                    dll_log("%s: %s @ 0x%08X has %s, wants %s", client::kLayouts[k].name, x.name, (unsigned)(base + x.rva),
                            c == SiteCheck::Unreadable ? "unreadable" : hex_bytes(cur, x.stubLen).c_str(),
                            hex_bytes(x.stub, x.stubLen).c_str());
                }
            }
            dll_log("no known client build matched: not hooking");
            set_state(HOOK_FAILED);
            return HOOK_FAILED;
        }
        if (!announced) {
            dll_log("waiting for the client code to unpack...");
            announced = true;
        }
        Sleep(100);
    }
    client::g_active = L;
    dll_log("client: %s", L->name);
    Site sites[2];
    sites_of(*L, sites);

    for (const Site& s : sites) {
        if (!install_site(base + s.rva, s)) {
            dll_log("failed to patch %s @ 0x%08X", s.name, (unsigned)(base + s.rva));
            set_state(HOOK_FAILED);
            return HOOK_FAILED;
        }
        dll_log("hooked %s @ 0x%08X", s.name, (unsigned)(base + s.rva));
    }
    set_state(HOOK_ACTIVE);
    return HOOK_ACTIVE;
}

} // namespace nj
