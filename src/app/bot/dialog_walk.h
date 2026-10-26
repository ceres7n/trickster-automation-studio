#pragma once
// Talks an NPC through its dialog to one call, the way a player clicks it (the server's rules: quest_graph.h). Walk
// to the NPC, C2S 0x6F -> S2C 112 (the first message; its number is not on the wire: found by its text, Dialog::identify),
// then along a gate's path: the option that leads on is asked for (a message: C2S 0x72) or run (a check: C2S 0x7F, the
// S2C 115 that answers names the next message) and each answer is compared with the path; at its end the call itself
// goes out. The caller then waits for its own answer (S2C 132 / 150 / 210 / 219 ...) and calls close() (Exit).
// What the answers mean — the first message picked by the NPC's PreCheck, the branch each check took — is handed back
// in `learned` (a quest done so many times, ...). A check that takes another branch than the path's ends the walk:
// the plan is then made again with what was learned.
#include "app/bot/bot_actions.h"
#include "app/quest_graph.h"
#include <string>
#include <vector>

namespace nj {

class DialogWalk {
public:
    enum Phase { Idle, Going, Opening, Stepping, Calling, Sent, Closing, Done, Failed };
    void reset();
    // To the NPC (Atlas::npcs() index) and its call func(args) through `gate` (null: right after the dialog opens).
    void begin(int npc, uint8_t func, const std::vector<uint16_t>& args, const QGate* gate, const std::string& what);
    bool busy() const { return phase_ != Idle && phase_ != Done && phase_ != Failed; }
    bool sent() const { return phase_ == Sent; }
    bool done() const { return phase_ == Done; }
    bool failed() const { return phase_ == Failed; }
    int  phase() const { return phase_; }
    int  npc() const { return npc_; }
    // Act = a request in c.out; Wait = on the way / waiting for an answer; Pass = idle, done, failed or sent (the
    // caller's turn: it watches for its answer, then close()).
    Step step(BotCtx& c);
    void close() { if (phase_ == Sent) phase_ = Closing; }
    void interrupted() { nav_.interrupted(); }
    uint64_t sentAt() const { return sentAt_; }
    uint16_t npc_eid() const { return eid_; }

    std::string status;                 // what it is doing
    std::string why;                    // Failed: why
    std::vector<QCond> learned;         // what the server's answers told
    bool deviated = false;              // the dialog went another way than the path (learned says why)
    bool lost = false;                  // Failed: no way to the NPC (the navigator's)
    bool absent = false;                // Failed: the NPC is not in its map (the server did not put it there)
    std::string skipped;                // a check the server does not do (QuestGraph::server_lacks) the walk went past

private:
    Step fail(BotCtx& c, const std::string& w);
    void learn(const std::vector<QCond>& cs);
    int  identify_first(const BotCtx& c) const;

    Phase    phase_ = Idle;
    int      npc_ = -1;
    uint8_t  func_ = 0;
    std::vector<uint16_t> args_;
    std::vector<int> path_;
    std::string what_;
    size_t   idx_ = 0;                  // where on the path
    bool     waiting_ = false;          // a 0x72 / 0x7F of the path went out
    int      expect_ = -1;              // the message it should bring
    const DialogOpt* stepOpt_ = nullptr;    // the check run (null: a link)
    uint16_t eid_ = 0;
    uint64_t at_ = 0, sentAt_ = 0, lostSince_ = 0;
    int      refused_ = 0;              // S2C 113: too far
    bool     failAfterClose_ = false;
    Navigator nav_;
};

} // namespace nj
