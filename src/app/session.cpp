#include "app/session.h"
#include "app/client_proc.h"
#include "app/text_util.h"
#include <fstream>

namespace nj {

namespace {
constexpr DWORD kConnectTimeoutMs = 30000;
constexpr DWORD kProcAccess = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                              PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_TERMINATE | SYNCHRONIZE;
}

Session::Session(uint32_t id, const GameData* gameData)
    : uid(id), world(std::make_unique<WorldModel>(gameData)) {}

Session::~Session()
{
    close();
    if (proc_) CloseHandle(proc_);
}

const char* Session::state_name() const
{
    switch (state) {
    case State::Starting:     return "starting";
    case State::Injecting:    return "injecting";
    case State::Connecting:   return "connecting";
    case State::Live:         return "live";
    case State::Reconnecting: return "reconnecting";
    case State::Exited:       return "client exited";
    case State::Failed:       return "failed";
    case State::Closed:       return "closed";
    case State::Offline:      return "offline (capture)";
    }
    return "?";
}

void Session::launch(const Config& cfg, const Account& acc)
{
    label = acc.label + " (" + acc.login + ")";
    thread_ = std::thread(&Session::worker, this, cfg, acc, 0u, true);
}

void Session::attach(const Config& cfg, uint32_t attachPid)
{
    label = fmt("PID %u", attachPid);
    pid = attachPid;
    thread_ = std::thread(&Session::worker, this, cfg, Account{}, attachPid, false);
}

void Session::close()
{
    stop_ = true;
    if (thread_.joinable()) {
        // Keep cancelling: the worker may enter its blocking ReadFile just after a cancel.
        HANDLE t = (HANDLE)thread_.native_handle();
        while (WaitForSingleObject(t, 100) == WAIT_TIMEOUT) {
            CancelSynchronousIo(t);
            std::lock_guard<std::mutex> g(cmdMu_);
            if (evt_ != INVALID_HANDLE_VALUE) CancelIoEx(evt_, nullptr);
        }
        thread_.join();
    }
    close_pipes();
}

bool Session::kill_client()
{
    std::lock_guard<std::mutex> g(mu_);
    return proc_ && TerminateProcess(proc_, 1);
}

void Session::set_status(State s, const std::string& text)
{
    std::lock_guard<std::mutex> g(mu_);
    sharedState_ = s;
    sharedStatus_ = text;
}

bool Session::sleep_ms(DWORD ms)
{
    for (DWORD t = 0; t < ms; t += 50) {
        if (stop_) return false;
        Sleep(50);
    }
    return !stop_;
}

bool Session::process_exited()
{
    std::lock_guard<std::mutex> g(mu_);
    return !proc_ || WaitForSingleObject(proc_, 0) == WAIT_OBJECT_0;
}

void Session::close_pipes()
{
    std::lock_guard<std::mutex> g(cmdMu_);
    if (evt_ != INVALID_HANDLE_VALUE) { CloseHandle(evt_); evt_ = INVALID_HANDLE_VALUE; }
    if (cmd_ != INVALID_HANDLE_VALUE) { CloseHandle(cmd_); cmd_ = INVALID_HANDLE_VALUE; }
}

void Session::worker(Config cfg, Account acc, uint32_t attachPid, bool doLaunch)
{
    auto log = [this](const std::string& s) { app_log("[%s] %s", label.c_str(), s.c_str()); };
    std::string err;
    uint32_t myPid = attachPid;

    if (doLaunch) {
        set_status(State::Starting, "launching the client");
        if (!cfg.preLaunch.empty()) {
            if (run_hidden(cfg.preLaunch, 10000, err)) log("pre-launch command ok");
            else log("pre-launch command: " + err);
        }
        PROCESS_INFORMATION pi{};
        if (!launch_client(cfg, acc.login, pi, err)) {
            log(err);
            set_status(State::Failed, err);
            return;
        }
        CloseHandle(pi.hThread);
        myPid = pi.dwProcessId;
        {
            std::lock_guard<std::mutex> g(mu_);
            proc_ = pi.hProcess;
            sharedPid_ = myPid;
        }
        log(fmt("client started, PID %u", myPid));
        set_status(State::Starting, "waiting for the client to unpack");
        if (!sleep_ms(cfg.unpackWaitMs)) return set_status(State::Closed, "closed");
        HANDLE p;
        { std::lock_guard<std::mutex> g(mu_); p = proc_; }
        redirect_client(p, cfg, log);
    } else {
        HANDLE p = OpenProcess(kProcAccess, FALSE, attachPid);
        if (!p) {
            err = fmt("OpenProcess(%u) failed (%lu) - run Trickster Automation Studio as administrator if the client is elevated",
                      attachPid, GetLastError());
            log(err);
            return set_status(State::Failed, err);
        }
        std::lock_guard<std::mutex> g(mu_);
        proc_ = p;
        sharedPid_ = attachPid;
    }

    set_status(State::Injecting, "injecting the hook DLL");
    if (process_has_module(myPid, "tas_hook.dll")) {
        log("hook DLL already loaded, reconnecting to it");
    } else {
        HANDLE p;
        { std::lock_guard<std::mutex> g(mu_); p = proc_; }
        if (!inject_dll(p, cfg.dllPath, err)) {
            log(err);
            return set_status(State::Failed, err);
        }
        log("hook DLL injected");
    }

    for (;;) {
        set_status(State::Connecting, "connecting to the hook DLL");
        if (!connect_pipes()) {
            if (stop_) return set_status(State::Closed, "closed");
            if (process_exited()) return set_status(State::Exited, "the client exited");
            log("could not open the pipes (another Trickster Automation Studio connected to this client?)");
            return set_status(State::Failed, "pipes unavailable");
        }
        set_status(State::Live, "live");
        read_loop();
        close_pipes();
        if (stop_) return set_status(State::Closed, "closed");
        if (process_exited()) {
            log("the client exited");
            return set_status(State::Exited, "the client exited");
        }
        log("link lost, reconnecting");
        set_status(State::Reconnecting, "link lost, reconnecting");
        if (!sleep_ms(500)) return set_status(State::Closed, "closed");
    }
}

bool Session::connect_pipes()
{
    uint32_t p;
    { std::lock_guard<std::mutex> g(mu_); p = sharedPid_; }
    char evtName[96], cmdName[96];
    pipe_name(evtName, sizeof(evtName), p, "evt");
    pipe_name(cmdName, sizeof(cmdName), p, "cmd");

    auto open = [&](const char* name, DWORD access, HANDLE& out) {
        const DWORD start = GetTickCount();
        while (!stop_ && GetTickCount() - start < kConnectTimeoutMs && !process_exited()) {
            HANDLE h = CreateFileA(name, access, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) { out = h; return true; }
            if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeA(name, 500);
            else Sleep(250);
        }
        return false;
    };

    HANDLE e = INVALID_HANDLE_VALUE, c = INVALID_HANDLE_VALUE;
    if (!open(evtName, GENERIC_READ, e)) return false;
    if (!open(cmdName, GENERIC_WRITE, c)) { CloseHandle(e); return false; }
    std::lock_guard<std::mutex> g(cmdMu_);
    evt_ = e;
    cmd_ = c;
    return true;
}

void Session::read_loop()
{
    HANDLE h;
    { std::lock_guard<std::mutex> g(cmdMu_); h = evt_; }
    MsgHeader hdr{};
    std::vector<uint8_t> body;
    while (!stop_ && pipe_read_msg(h, hdr, body)) {
        std::lock_guard<std::mutex> g(mu_);
        inbox_.emplace_back(hdr.type, std::move(body));
        body = {};
    }
}

bool Session::send(uint32_t ctx, uint16_t op, uint16_t param, const std::vector<uint8_t>& data, std::string& err)
{
    CmdSend c{};
    SendRecord r;
    {
        std::lock_guard<std::mutex> g(cmdMu_);
        if (cmd_ == INVALID_HANDLE_VALUE) { err = "not connected"; return false; }
        c.reqId   = nextReq_++;
        c.ctx     = ctx;
        c.opcode  = op;
        c.param   = param;
        c.dataLen = (uint32_t)data.size();
        if (!pipe_write_msg(cmd_, CMD_SEND, &c, sizeof(c), data.data(), (uint32_t)data.size())) {
            err = fmt("pipe write failed (%lu)", GetLastError());
            return false;
        }
    }
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    r.reqId = c.reqId;
    r.ctx   = ctx;
    r.op    = op;
    r.len   = c.dataLen;
    r.when  = filetime_local_hms((uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    sends.push_back(std::move(r));
    return true;
}

void Session::ingest(CapPacket&& p)
{
    OpStat& st = opStats[uint32_t(p.hdr.dir) << 16 | p.hdr.opcode];
    ++st.count;
    st.bytes += p.hdr.size;
    st.lastSeq = p.hdr.seq;
    if (p.hdr.dir == DIR_C2S && !(p.hdr.flags & PF_INJECTED)) ++c2sConns[p.hdr.ctx];
    if (p.hdr.dir == DIR_C2S && p.hdr.ctx) {
        // requests only the GameServer handles: their connection is the game link (the community
        // link to the SystemServer also sends C2S, so "the last C2S connection" may not be it).
        // 0x009C = CMD_ENTER_ZONE, the first packet of the new link after a map change.
        switch (p.hdr.opcode) {
        case 0x0018: case 0x001D: case 0x0021: case 0x0040: case 0x0057: case 0x00E0: case 0x00EE: case 0x01D4:
        case 0x009C: case 0x0016: case 0x02F7: case 0x0046: case 0x0048: case 0x004F: case 0x006F: case 0x0072:
        case 0x0075: case 0x007F: case 0x008A: case 0x008B: case 0x039F: case 0x00A4:
        case 0x002C: case 0x0030: case 0x0031: case 0x0051: case 0x0054:
            gameCtx = p.hdr.ctx;
            break;
        default:
            break;
        }
    }
    // a new world entry / a map change (the client reconnects): the next game C2S names the new link
    if (p.hdr.dir == DIR_S2C && (p.hdr.opcode == 0x00E6 || p.hdr.opcode == 0x009A)) gameCtx = 0;
    world->feed(p);

    packets.push_back(std::move(p));
    while (packets.size() > kMaxPackets) {
        packets.pop_front();
        ++firstIndex;
    }
}

// The export format (NeoApp::export_view), one packet per line, tab-separated:
//   seq  time  dir(C>S|S>C)  0xOPCODE  size  flags(i t r, or -)  name  payload hex
bool Session::load_capture(const std::string& path, std::string& err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open " + path; return false; }
    label = "capture " + path.substr(path.find_last_of("\\/") + 1);
    std::string line;
    size_t n = 0, bad = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        for (size_t a = 0;;) {
            size_t t = line.find('\t', a);
            f.push_back(line.substr(a, t == std::string::npos ? std::string::npos : t - a));
            if (t == std::string::npos) break;
            a = t + 1;
        }
        uint32_t seq = 0, op = 0, size = 0;
        std::string hexErr;
        CapPacket p;
        if (f.size() < 8 || !parse_uint(f[0], seq) || !parse_uint(f[3], op) || !parse_uint(f[4], size) ||
            !parse_hex(f[7], p.data, hexErr)) {
            ++bad;
            continue;
        }
        p.hdr.seq = seq;
        p.hdr.dir = f[2] == "C>S" ? DIR_C2S : DIR_S2C;
        p.hdr.opcode = (uint16_t)op;
        p.hdr.size = size;
        p.hdr.dataLen = (uint32_t)p.data.size();
        if (f[5].find('i') != std::string::npos) p.hdr.flags |= PF_INJECTED;
        if (f[5].find('t') != std::string::npos) p.hdr.flags |= PF_TRUNCATED;
        if (f[5].find('r') != std::string::npos) p.hdr.flags |= PF_REPLAY;
        lastSeq = seq;
        ingest(std::move(p));
        ++n;
    }
    if (!n && bad) { err = path + ": not an exported capture"; return false; }
    {
        std::lock_guard<std::mutex> g(mu_);
        sharedState_ = State::Offline;
        sharedStatus_ = fmt("offline: %zu packets", n);
    }
    poll();
    if (bad) app_log("[%s] %zu lines skipped (not in the export format)", label.c_str(), bad);
    app_log("[%s] loaded %zu packets", label.c_str(), n);
    return true;
}

void Session::poll()
{
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> in;
    {
        std::lock_guard<std::mutex> g(mu_);
        in.swap(inbox_);
        state  = sharedState_;
        status = sharedStatus_;
        pid    = sharedPid_ ? sharedPid_ : pid;
    }
    for (auto& m : in) handle(m.first, m.second);
}

void Session::handle(uint16_t type, const std::vector<uint8_t>& body)
{
    switch (type) {
    case EV_HELLO:
        if (body.size() < sizeof(EvHello)) return;
        memcpy(&hello, body.data(), sizeof(hello));
        haveHello = true;
        hookState = hello.hookState;
        replayDone = false;
        if (hello.version != kProtoVersion)
            app_log("[%s] hook DLL speaks protocol v%u, Trickster Automation Studio v%u - rebuild/restart the client",
                    label.c_str(), hello.version, kProtoVersion);
        break;

    case EV_PACKET: {
        if (body.size() < sizeof(EvPacket)) return;
        CapPacket p;
        memcpy(&p.hdr, body.data(), sizeof(EvPacket));
        if (p.hdr.seq <= lastSeq) return;   // already have it (history replayed on reconnect)
        lastSeq = p.hdr.seq;
        size_t n = body.size() - sizeof(EvPacket);
        if (n > p.hdr.dataLen) n = p.hdr.dataLen;
        p.data.assign(body.begin() + sizeof(EvPacket), body.begin() + sizeof(EvPacket) + n);
        ingest(std::move(p));
        break;
    }

    case EV_LOG:
        app_log("[%s] dll: %s", label.c_str(), std::string(body.begin(), body.end()).c_str());
        break;

    case EV_SEND_RESULT: {
        if (body.size() < sizeof(EvSendResult)) return;
        EvSendResult r;
        memcpy(&r, body.data(), sizeof(r));
        r.error[sizeof(r.error) - 1] = '\0';
        for (auto it = sends.rbegin(); it != sends.rend(); ++it) {
            if (it->reqId != r.reqId) continue;
            it->done = true;
            it->ok = r.ok != 0;
            it->error = r.error;
            break;
        }
        if (!r.ok) app_log("[%s] send #%u failed: %s", label.c_str(), r.reqId, r.error);
        break;
    }

    case EV_HOOK_STATE: {
        if (body.size() < sizeof(EvHookState)) return;
        EvHookState s;
        memcpy(&s, body.data(), sizeof(s));
        hookState = s.state;
        break;
    }

    case EV_AVATAR: {
        if (body.size() < sizeof(EvAvatar) || !world) return;
        EvAvatar a;
        memcpy(&a, body.data(), sizeof(a));
        world->feed_avatar(a);
        break;
    }

    case EV_REPLAY_DONE:
        replayDone = true;
        app_log("[%s] linked, %llu packets so far", label.c_str(), (unsigned long long)total_packets());
        break;

    case EV_LOST: {
        if (body.size() < sizeof(EvLost)) return;
        EvLost l;
        memcpy(&l, body.data(), sizeof(l));
        lostInDll += l.count;
        app_log("[%s] %llu packets fell out of the DLL history before they were read",
                label.c_str(), (unsigned long long)l.count);
        break;
    }
    }
}

} // namespace nj
