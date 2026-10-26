#pragma once
// One client process under Trickster Automation Studio: launch/attach, hook DLL injection, the pipe link
// and everything it has captured.
#include "app/bot/bot.h"
#include "app/config.h"
#include "app/world.h"
#include "shared/ipc.h"
#include <memory>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace nj {

struct CapPacket {
    EvPacket hdr{};
    std::vector<uint8_t> data;
};

struct SendRecord {
    uint32_t    reqId = 0;
    uint32_t    ctx = 0;
    uint16_t    op = 0;
    uint32_t    len = 0;
    std::string when;
    bool        done = false;
    bool        ok = false;
    std::string error;
};

struct OpStat {
    uint64_t count = 0;
    uint64_t bytes = 0;
    uint64_t lastSeq = 0;
};

class Session {
public:
    enum class State { Starting, Injecting, Connecting, Live, Reconnecting, Exited, Failed, Closed, Offline };
    static constexpr size_t kMaxPackets = 300000;

    Session(uint32_t uid, const GameData* gameData);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void launch(const Config& cfg, const Account& acc);
    void attach(const Config& cfg, uint32_t pid);
    void close();          // drops the link; the DLL keeps capturing inside the client
    bool load_capture(const std::string& path, std::string& err);   // an exported capture, offline
    bool kill_client();

    void poll();           // UI thread, once per frame: applies what the link received

    // C2S through the client. ctx 0 = the client's last C2S connection.
    bool send(uint32_t ctx, uint16_t op, uint16_t param, const std::vector<uint8_t>& data, std::string& err);

    const char* state_name() const;
    bool finished() const { return state == State::Exited || state == State::Failed || state == State::Closed ||
                                   state == State::Offline; }
    uint64_t total_packets() const { return firstIndex + packets.size(); }
    const CapPacket* packet_at(uint64_t absIndex) const
    {
        return absIndex >= firstIndex && absIndex < total_packets() ? &packets[size_t(absIndex - firstIndex)] : nullptr;
    }

    // ── UI-thread state (written by poll) ───────────────────────────────────
    const uint32_t uid;
    std::string    label;
    uint32_t       pid = 0;
    State          state = State::Starting;
    std::string    status;
    EvHello        hello{};
    bool           haveHello = false;
    uint32_t       hookState = HOOK_WAITING;
    bool           replayDone = false;
    uint64_t       lostInDll = 0;
    uint64_t       lastSeq = 0;
    std::deque<CapPacket> packets;
    uint64_t       firstIndex = 0;                 // absolute index of packets.front()
    std::map<uint32_t, OpStat>   opStats;          // dir << 16 | opcode
    std::map<uint32_t, uint64_t> c2sConns;         // ctx -> C2S packets seen on it
    uint32_t                     gameCtx = 0;      // the game-server link (0 = not seen yet -> auto)
    std::vector<SendRecord>      sends;
    std::unique_ptr<WorldModel>  world;            // fed every packet, in capture order
    Bot                          bot;              // phase 4 automation, ticked by NeoApp::update
    uint64_t                     botProfileFor = 0; // char uid whose profile the bot has loaded
    int                          bookSaved = -1;    // the Key Quest book's version last written (KeyQuestAction::book_version)
    uint64_t                     logWritten = 0;    // bot notes already appended to logs\bot_<char uid>.log (Bot::notes)

private:
    void worker(Config cfg, Account acc, uint32_t attachPid, bool doLaunch);
    bool connect_pipes();
    void read_loop();
    void set_status(State s, const std::string& text);
    bool sleep_ms(DWORD ms);               // false when stopping
    bool process_exited();
    void close_pipes();
    void handle(uint16_t type, const std::vector<uint8_t>& body);
    void ingest(CapPacket&& p);

    std::thread       thread_;
    std::atomic<bool> stop_{ false };

    std::mutex mu_;                        // inbox + shared state + proc_
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> inbox_;
    State       sharedState_ = State::Starting;
    std::string sharedStatus_;
    uint32_t    sharedPid_ = 0;
    HANDLE      proc_ = nullptr;

    std::mutex cmdMu_;                     // pipe handles
    HANDLE     evt_ = INVALID_HANDLE_VALUE;
    HANDLE     cmd_ = INVALID_HANDLE_VALUE;
    uint32_t   nextReq_ = 1;
};

} // namespace nj
