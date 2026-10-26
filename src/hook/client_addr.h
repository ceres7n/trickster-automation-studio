#pragma once
// The client builds the hook knows (IDB base 0x400000, RVAs from the client RE). hooks_install picks the one
// whose two capture sites hold their expected prologues (memory: kto-client-s2c-dispatchers).
#include <cstddef>
#include <cstdint>

namespace nj::client {

// How the S2C site gets its packet.
enum class S2CArgs : uint8_t {
    Direct,     // __thiscall(this, category, op, src, size)                   (Paula post_game_event)
    Cursor,     // __thiscall(this, category, param, op, cursor{src, size, ..}) (S2 NetworkManager::OnPacket)
};

struct Layout {
    const char*    name;
    // C2S: the outbound framer every C2S goes through, plaintext in:
    //   __stdcall(op, param, src, size, buf, cap, flags), ecx = connection + ctxToConn
    uint32_t       rvaC2SFrame;
    const uint8_t* c2sStub;
    uint32_t       c2sStubLen;      // 5..8 bytes of whole, position-independent instructions
    // S2C: where every decrypted S2C passes, plaintext payload
    uint32_t       rvaS2CEvent;
    const uint8_t* s2cStub;
    uint32_t       s2cStubLen;
    S2CArgs        s2cArgs;
    // Net_SendPacket_Raw  __thiscall(conn, op, param, src, size, buf, cap, flags), retn 1Ch, al = sent
    uint32_t       rvaNetSendRaw;
    uint32_t       frameCap;        // what the client's Net_Send_Data passes (with its TLS buffer)
    uint32_t       sendFlags;
    uint32_t       ctxToConn;       // the C2S site's ecx minus this = the connection
    // cDirectPlaygirl, the connection
    uint32_t       rvaConnVtable;
    uint32_t       connSocket;      // SOCKET, -1 once closed
    uint32_t       connState;       // 1 connected
    // The local avatar (a Mover): [world global] -> +worldAvatar (Build_Local_Avatar / Field_Enter set it).
    // Its +0x5C flags (& 7 = the move type while it moves), +0x6C int16 x, y (drawn) and +0x70 the move's end
    // (PathFindingMoveStrategy sub_403960 / sub_4030E0 write them). 0 = unknown for this build.
    uint32_t       rvaWorld;
    uint32_t       worldAvatar;
};
inline constexpr uint32_t kAvatarFlags = 0x5C, kAvatarPos = 0x6C, kAvatarDest = 0x70;

// Client_live\Trickster.bin (KTO Jan-2014 "Paula", md5 46ee494c).
inline constexpr uint8_t kPaulaC2S[] = { 0x51, 0x56, 0x8B, 0x74, 0x24, 0x20 };              // push ecx; push esi; mov esi,[esp+20h]
inline constexpr uint8_t kPaulaS2C[] = { 0x8B, 0x44, 0x24, 0x10, 0x8B, 0x54, 0x24, 0x0C };  // mov eax,[esp+10h]; mov edx,[esp+0Ch]

// Client_live\TricksterS2.exe (Season 2, md5 a69cb26e). The same framer (0x6251F0) and Net_SendPacket_Raw
// (0x58AF80, `lea ecx,[conn+28h]` before the framer). Its deframer (0x58B270) queues each decrypted packet as a
// 28-byte object {+0 data, +4 size, +8 cursor{data, size, size, 0}, +24 u16 param (header word 4), +26 u16 op
// (header word 2)} and the game thread's pump (0x589A30) hands it to the connection's owner:
// NetworkManager::OnPacket (vtable 0xAB5F90 slot 0, 0x58AC50)(category = the connection's 0 / 1 / 2, param, op,
// cursor). The connection (ctor 0x58B080): vtable 0xAB6140, +0x24 SOCKET, +0x28 / +0x29 the cipher states,
// +0x10040 state (1 connected; 2 / 3 -> OnPacket 1400 / 1401), +0x10044 / +0x10048 its send lock.
inline constexpr uint8_t kS2C2S[] = { 0x51, 0x56, 0x8B, 0x74, 0x24, 0x20 };                 // push ecx; push esi; mov esi,[esp+20h]
inline constexpr uint8_t kS2S2C[] = { 0x8B, 0x54, 0x24, 0x0C, 0x0F, 0xB7, 0xC2 };           // mov edx,[esp+0Ch]; movzx eax,dx

// The world global: Paula g_ppGameWorld 0x10F2C60, S2 0xCE1D00 (both read [g]+0x568 -> +0x5C / +0x6C in their
// stop-key handler, sub_6B9F80 / sub_429E40, which sends the same C2S 0x18 [pos][pos] the bot's resync does).
inline constexpr Layout kLayouts[] = {
    { "Paula (Trickster.bin, KTO Jan-2014)",
      0x411FF0, kPaulaC2S, sizeof(kPaulaC2S), 0x42A820, kPaulaS2C, sizeof(kPaulaS2C), S2CArgs::Direct,
      0x42A210, 0x8000, 0x0F, 0x8, 0x9EE0AC, 0x4, 0x8130, 0xCF2C60, 0x568 },
    { "Season 2 (TricksterS2.exe)",
      0x2251F0, kS2C2S, sizeof(kS2C2S), 0x18AC50, kS2S2C, sizeof(kS2S2C), S2CArgs::Cursor,
      0x18AF80, 0x8000, 0x0F, 0x28, 0x6B6140, 0x24, 0x10040, 0x8E1D00, 0x568 },
};
inline constexpr size_t kLayoutCount = sizeof(kLayouts) / sizeof(kLayouts[0]);

// The layout hooks_install matched (null until then; set before the hooks go live).
inline const Layout* volatile g_active = nullptr;
inline const Layout* active() { return g_active; }

} // namespace nj::client
