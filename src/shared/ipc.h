#pragma once
// Wire protocol between TricksterAutomationStudio.exe and tas_hook.dll (inside the client).
//
// Two byte-mode named pipes per client process, both created by the DLL:
//   \\.\pipe\tas-<pid>-evt   DLL -> app   (hello, packets, logs, send results)
//   \\.\pipe\tas-<pid>-cmd   app -> DLL   (commands)
// Every message = MsgHeader + `size` body bytes. One writer per pipe.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <windows.h>

namespace nj {

constexpr uint32_t kProtoVersion = 1;
constexpr uint32_t kMaxMsgBody   = 1u << 20;   // sanity cap for a single message

inline void pipe_name(char* out, size_t cap, uint32_t pid, const char* which)
{
    snprintf(out, cap, "\\\\.\\pipe\\tas-%u-%s", pid, which);
}

#pragma pack(push, 1)
struct MsgHeader {
    uint32_t size;      // body bytes after this header
    uint16_t type;      // MsgType
    uint16_t reserved;
};

enum MsgType : uint16_t {
    // DLL -> app
    EV_HELLO       = 1,     // EvHello
    EV_PACKET      = 2,     // EvPacket + payload
    EV_LOG         = 3,     // text
    EV_SEND_RESULT = 4,     // EvSendResult
    EV_HOOK_STATE  = 5,     // EvHookState
    EV_REPLAY_DONE = 6,     // EvReplayDone: every packet captured before this connection was sent
    EV_LOST        = 7,     // EvLost: packets dropped from the history before the app read them
    EV_AVATAR      = 8,     // EvAvatar: where the client draws our own character (on a change, else each second)
    // app -> DLL
    CMD_SEND       = 100,   // CmdSend + payload
};

enum HookState : uint32_t {
    HOOK_WAITING = 0,   // waiting for the client code to match the expected prologues
    HOOK_ACTIVE  = 1,
    HOOK_FAILED  = 2,
};

enum PacketDir : uint8_t { DIR_C2S = 0, DIR_S2C = 1 };

enum PacketFlags : uint8_t {
    PF_INJECTED  = 0x01,    // sent by Trickster Automation Studio (CMD_SEND), not by the client itself
    PF_TRUNCATED = 0x02,    // payload longer than what was kept
    PF_REPLAY    = 0x04,    // captured before this pipe connection (history replay)
};

struct EvHello {
    uint32_t version;
    uint32_t pid;
    uint32_t moduleBase;
    uint32_t hookState;
    char     exePath[260];
};

struct EvPacket {
    uint64_t seq;       // capture order, from 1, per client process
    uint64_t fileTime;  // UTC FILETIME at capture
    uint8_t  dir;       // PacketDir
    uint8_t  flags;     // PacketFlags
    uint16_t opcode;
    uint16_t param;     // C2S: the frame header param (the client sends 0); S2C: the connection's category
    uint16_t reserved;
    uint32_t ctx;       // C2S: the framer's ecx (what CmdSend::ctx takes: connection + 8 Paula / + 0x28 S2);
                        // S2C: `this` of post_game_event (Paula) / NetworkManager::OnPacket (S2)
    uint32_t size;      // real payload length
    uint32_t dataLen;   // payload bytes that follow (< size when truncated)
};

struct EvHookState  { uint32_t state; };
struct EvSendResult { uint32_t reqId; uint8_t ok; char error[123]; };
struct EvReplayDone { uint64_t lastSeq; };
struct EvLost       { uint64_t count; };

// The local avatar as the client moves it (client_addr.h: the world global -> +0x568 the avatar). The client
// path-finds every move the server orders for it; a move it finds no way for leaves it standing.
struct EvAvatar {
    uint64_t fileTime;  // UTC FILETIME of the read
    uint32_t actor;     // the avatar object (0 = none: not in a map, or the client build has no known layout)
    int16_t  x, y;      // avatar +0x6C: where it is drawn (units)
    int16_t  dx, dy;    // avatar +0x70: where its move goes
    uint32_t flags;     // avatar +0x5C: & 7 = the move type while it moves (0 = standing)
};

struct CmdSend {
    uint32_t reqId;
    uint32_t ctx;       // an EvPacket C2S ctx; 0 = the connection of the client's last C2S
    uint16_t opcode;
    uint16_t param;
    uint32_t dataLen;   // payload bytes that follow
};
#pragma pack(pop)

// ── blocking pipe I/O ───────────────────────────────────────────────────────
inline bool pipe_write_all(HANDLE h, const void* p, DWORD n)
{
    const uint8_t* b = static_cast<const uint8_t*>(p);
    while (n) {
        DWORD w = 0;
        if (!WriteFile(h, b, n, &w, nullptr) || w == 0) return false;
        b += w; n -= w;
    }
    return true;
}

inline bool pipe_read_all(HANDLE h, void* p, DWORD n)
{
    uint8_t* b = static_cast<uint8_t*>(p);
    while (n) {
        DWORD r = 0;
        if (!ReadFile(h, b, n, &r, nullptr) || r == 0) return false;
        b += r; n -= r;
    }
    return true;
}

// One WriteFile per message (header + up to two body parts).
inline bool pipe_write_msg(HANDLE h, uint16_t type, const void* a, uint32_t an,
                           const void* b = nullptr, uint32_t bn = 0)
{
    std::vector<uint8_t> buf(sizeof(MsgHeader) + an + bn);
    MsgHeader hdr{ an + bn, type, 0 };
    memcpy(buf.data(), &hdr, sizeof(hdr));
    if (an) memcpy(buf.data() + sizeof(hdr), a, an);
    if (bn) memcpy(buf.data() + sizeof(hdr) + an, b, bn);
    return pipe_write_all(h, buf.data(), (DWORD)buf.size());
}

inline bool pipe_read_msg(HANDLE h, MsgHeader& hdr, std::vector<uint8_t>& body)
{
    if (!pipe_read_all(h, &hdr, sizeof(hdr))) return false;
    if (hdr.size > kMaxMsgBody) return false;
    body.resize(hdr.size);
    return hdr.size == 0 || pipe_read_all(h, body.data(), hdr.size);
}

} // namespace nj
