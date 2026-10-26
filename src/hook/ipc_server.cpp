#include "hook/ipc_server.h"
#include "hook/avatar.h"
#include "hook/hooks.h"
#include "hook/packet_store.h"
#include "hook/sender.h"

namespace nj {

namespace {

constexpr DWORD kKeepAliveMs = 1000;   // a write this often notices a vanished app
constexpr DWORD kAvatarReadMs = 100;   // the avatar is read this often, sent when it changed
constexpr DWORD kAvatarSameMs = 1000;  // and at least this often while it stays the same

bool same_avatar(const EvAvatar& a, const EvAvatar& b)
{
    return a.actor == b.actor && a.x == b.x && a.y == b.y && a.dx == b.dx && a.dy == b.dy && a.flags == b.flags;
}

HANDLE make_pipe(const char* which, DWORD access)
{
    char name[96];
    pipe_name(name, sizeof(name), GetCurrentProcessId(), which);
    return CreateNamedPipeA(name, access, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                            1, 1u << 20, 1u << 20, 0, nullptr);
}

bool wait_client(HANDLE h)
{
    return ConnectNamedPipe(h, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
}

void serve_events(HANDLE h)
{
    EvHello hello{};
    hello.version    = kProtoVersion;
    hello.pid        = GetCurrentProcessId();
    hello.moduleBase = (uint32_t)(uintptr_t)GetModuleHandleA(nullptr);
    hello.hookState  = hooks_state();
    GetModuleFileNameA(nullptr, hello.exePath, sizeof(hello.exePath) - 1);
    if (!pipe_write_msg(h, EV_HELLO, &hello, sizeof(hello))) return;

    // Everything captured so far is history: replay it, then go live.
    const uint64_t replayEnd = store().next_seq();
    uint64_t from = 1;
    bool replayDone = false;
    DWORD lastWrite = GetTickCount();
    DWORD avatarReadAt = 0, avatarSentAt = 0;
    EvAvatar lastAvatar{};
    bool avatarSent = false;
    std::vector<RecordPtr> recs;
    std::vector<Event> events;

    for (;;) {
        if (replayDone && GetTickCount() - avatarReadAt >= kAvatarReadMs) {
            avatarReadAt = GetTickCount();
            EvAvatar a;
            if (read_avatar(a) && (!avatarSent || !same_avatar(a, lastAvatar) || avatarReadAt - avatarSentAt >= kAvatarSameMs)) {
                if (!pipe_write_msg(h, EV_AVATAR, &a, sizeof(a))) return;
                lastAvatar = a;
                avatarSent = true;
                avatarSentAt = avatarReadAt;
                lastWrite = avatarReadAt;
            }
        }

        uint64_t lost = 0;
        recs.clear();
        store().collect(from, recs, 1u << 20, lost);
        if (lost) {
            EvLost l{ lost };
            if (!pipe_write_msg(h, EV_LOST, &l, sizeof(l))) return;
        }
        for (const RecordPtr& r : recs) {
            EvPacket hdr = r->hdr;
            if (hdr.seq < replayEnd) hdr.flags |= PF_REPLAY;
            if (!pipe_write_msg(h, EV_PACKET, &hdr, sizeof(hdr), r->data.data(), (uint32_t)r->data.size()))
                return;
        }
        if (!replayDone && from >= replayEnd) {
            EvReplayDone d{ replayEnd - 1 };
            if (!pipe_write_msg(h, EV_REPLAY_DONE, &d, sizeof(d))) return;
            replayDone = true;
        }

        events.clear();
        store().take_events(events);
        for (const Event& e : events)
            if (!pipe_write_msg(h, e.type, e.body.data(), (uint32_t)e.body.size())) return;

        if (!recs.empty() || !events.empty()) {
            lastWrite = GetTickCount();
            continue;
        }
        if (GetTickCount() - lastWrite >= kKeepAliveMs) {
            EvHookState s{ hooks_state() };
            if (!pipe_write_msg(h, EV_HOOK_STATE, &s, sizeof(s))) return;
            lastWrite = GetTickCount();
        }
        sender_watchdog();
        WaitForSingleObject(store().wake_event(), replayDone ? kAvatarReadMs : 200);
    }
}

void serve_commands(HANDLE h)
{
    MsgHeader hdr{};
    std::vector<uint8_t> body;
    while (pipe_read_msg(h, hdr, body)) {
        if (hdr.type == CMD_SEND && body.size() >= sizeof(CmdSend)) {
            CmdSend c;
            memcpy(&c, body.data(), sizeof(c));
            if (sizeof(CmdSend) + c.dataLen > body.size()) continue;
            SendJob job;
            job.reqId = c.reqId;
            job.ctx   = c.ctx;
            job.op    = c.opcode;
            job.param = c.param;
            job.data.assign(body.begin() + sizeof(CmdSend), body.begin() + sizeof(CmdSend) + c.dataLen);
            sender_queue(std::move(job));
        }
    }
}

DWORD WINAPI events_thread(LPVOID)
{
    for (;;) {
        HANDLE h = make_pipe("evt", PIPE_ACCESS_OUTBOUND);
        if (h == INVALID_HANDLE_VALUE) { Sleep(1000); continue; }
        if (wait_client(h)) serve_events(h);
        DisconnectNamedPipe(h);
        CloseHandle(h);
    }
}

DWORD WINAPI commands_thread(LPVOID)
{
    for (;;) {
        HANDLE h = make_pipe("cmd", PIPE_ACCESS_INBOUND);
        if (h == INVALID_HANDLE_VALUE) { Sleep(1000); continue; }
        if (wait_client(h)) serve_commands(h);
        DisconnectNamedPipe(h);
        CloseHandle(h);
    }
}

} // namespace

void ipc_start()
{
    CloseHandle(CreateThread(nullptr, 0, events_thread, nullptr, 0, nullptr));
    CloseHandle(CreateThread(nullptr, 0, commands_thread, nullptr, 0, nullptr));
}

} // namespace nj
