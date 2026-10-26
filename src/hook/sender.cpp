#include "hook/sender.h"
#include "hook/client_addr.h"
#include "hook/hooks.h"
#include "hook/packet_store.h"
#include <deque>
#include <string>

namespace nj {

namespace {

// Net_SendPacket_Raw is __thiscall; __fastcall with a dummy edx has the same shape
// (ecx = conn, rest on the stack, callee pops 0x1C).
typedef uint8_t(__fastcall* NetSendRawFn)(void* conn, void* edx, int op, int param, const void* src,
                                          int size, void* buf, int cap, int flags);

struct PendingJob {
    SendJob job;
    DWORD   queuedAt;
};

SRWLOCK                g_lock = SRWLOCK_INIT;
std::deque<PendingJob> g_jobs;
HWND                   g_hwnd = nullptr;
WNDPROC                g_origProc = nullptr;
bool                   g_unicode = false;
UINT                   g_execMsg = 0;

constexpr DWORD kWatchdogMs = 2000;

bool read_u32(uintptr_t addr, uint32_t& v)
{
    __try {
        v = *(volatile uint32_t*)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 1 = sent, 0 = the client refused, -1 = it faulted.
int call_send_raw(uintptr_t fn, uintptr_t conn, uint16_t op, uint16_t param,
                  const uint8_t* src, uint32_t size, uint8_t* frame, uint32_t cap, uint32_t flags)
{
    __try {
        return ((NetSendRawFn)fn)((void*)conn, nullptr, op, param, src, (int)size, frame, (int)cap, (int)flags) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void post_result(uint32_t reqId, bool ok, const char* error)
{
    EvSendResult r{};
    r.reqId = reqId;
    r.ok = ok ? 1 : 0;
    if (error) strncpy(r.error, error, sizeof(r.error) - 1);
    store().post_event(EV_SEND_RESULT, &r, sizeof(r));
}

void exec_job(const SendJob& j, const char* where)
{
    const uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    const client::Layout* L = client::active();
    if (!L) return post_result(j.reqId, false, "the hook is not active (no client build matched yet)");
    const uint32_t ctx = j.ctx ? j.ctx : store().last_c2s_ctx();
    if (ctx < L->ctxToConn + 1) return post_result(j.reqId, false, "no client connection seen yet");
    if (j.data.size() + 11 > L->frameCap) return post_result(j.reqId, false, "payload too large");

    // Refuse a connection that is gone (login -> game hand-off, relog) instead of calling
    // into a freed object. ctx = the C2S site's ecx = connection + ctxToConn.
    const uintptr_t conn = ctx - L->ctxToConn;
    uint32_t vtbl = 0, sock = 0, state = 0;
    if (!read_u32(conn, vtbl) || !read_u32(conn + L->connSocket, sock) || !read_u32(conn + L->connState, state))
        return post_result(j.reqId, false, "connection object unreadable");
    if (vtbl != base + L->rvaConnVtable) return post_result(j.reqId, false, "connection object is gone");
    if (sock == 0xFFFFFFFF || state != 1) return post_result(j.reqId, false, "connection is closed");

    std::vector<uint8_t> frame(L->frameCap);
    t_injecting = true;
    int rc = call_send_raw(base + L->rvaNetSendRaw, conn, j.op, j.param, j.data.empty() ? nullptr : j.data.data(),
                           (uint32_t)j.data.size(), frame.data(), L->frameCap, L->sendFlags);
    t_injecting = false;

    if (rc == 1) {
        if (where) dll_log("req %u sent from %s", j.reqId, where);
        post_result(j.reqId, true, nullptr);
    } else {
        post_result(j.reqId, false, rc == 0 ? "Net_SendPacket_Raw returned 0" : "the client faulted while sending");
    }
}

bool pop_job(PendingJob& out, bool onlyOverdue)
{
    AcquireSRWLockExclusive(&g_lock);
    bool got = false;
    if (!g_jobs.empty() && (!onlyOverdue || GetTickCount() - g_jobs.front().queuedAt >= kWatchdogMs)) {
        out = std::move(g_jobs.front());
        g_jobs.pop_front();
        got = true;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return got;
}

LRESULT CALLBACK exec_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == g_execMsg) {
        PendingJob p;
        while (pop_job(p, false)) exec_job(p.job, nullptr);
        return 0;
    }
    WNDPROC orig = g_origProc;
    LRESULT r = g_unicode ? CallWindowProcW(orig, h, m, w, l) : CallWindowProcA(orig, h, m, w, l);
    if (m == WM_NCDESTROY && h == g_hwnd) g_hwnd = nullptr;   // window recreated -> find again
    return r;
}

struct FindCtx { HWND best; LONG bestArea; };

BOOL CALLBACK find_window_proc(HWND h, LPARAM lp)
{
    auto* f = (FindCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT rc{};
    GetClientRect(h, &rc);
    LONG area = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (area > f->bestArea) { f->best = h; f->bestArea = area; }
    return TRUE;
}

// The client's main window, subclassed once.
HWND game_window()
{
    if (g_hwnd && IsWindow(g_hwnd)) return g_hwnd;
    FindCtx f{ nullptr, 0 };
    EnumWindows(find_window_proc, (LPARAM)&f);
    if (!f.best) return nullptr;
    if (!g_execMsg) g_execMsg = RegisterWindowMessageA("TAS.ExecSend");
    g_unicode = IsWindowUnicode(f.best) != FALSE;
    LONG_PTR prev = g_unicode ? SetWindowLongPtrW(f.best, GWLP_WNDPROC, (LONG_PTR)exec_wndproc)
                              : SetWindowLongPtrA(f.best, GWLP_WNDPROC, (LONG_PTR)exec_wndproc);
    if (!prev) return nullptr;
    g_origProc = (WNDPROC)prev;
    g_hwnd = f.best;
    char title[128] = {};
    GetWindowTextA(f.best, title, sizeof(title));
    dll_log("sends run on the game window thread (hwnd %p \"%s\")", (void*)f.best, title);
    return g_hwnd;
}

} // namespace

void sender_queue(SendJob&& job)
{
    HWND h = game_window();
    if (!h) {
        exec_job(job, "the pipe thread (no game window yet)");
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_jobs.push_back(PendingJob{ std::move(job), GetTickCount() });
    ReleaseSRWLockExclusive(&g_lock);
    PostMessageA(h, g_execMsg, 0, 0);
}

void sender_watchdog()
{
    PendingJob p;
    while (pop_job(p, true)) exec_job(p.job, "a worker thread (the game thread did not run it in 2 s)");
}

} // namespace nj
