#pragma once
#include "app/actions.h"
#include "app/bot/bot_actions.h"
#include "app/client_proc.h"
#include "app/config.h"
#include "app/game_data.h"
#include "app/packet_db.h"
#include "app/session.h"
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

struct ImFont;

namespace nj {

class NeoApp {
public:
    NeoApp();
    ~NeoApp();
    void update();            // every loop pass, also while the window is hidden: links + bots
    void frame();             // between ImGui::NewFrame and ImGui::Render

    // Command line: --launch <login> (repeatable), --attach <pid>, --open <capture.txt>,
    //               --tab <home|settings|overview|heal|loot|attack|drill|quest|keyquest|story|hunt|travel|supply|sell|
    //                      storage|session|world|character|packets|opcodes|send|log>, --shot <file.png> (main.cpp)
    void handle_command_line(const char* cmdLine);

    ImFont* monoFont = nullptr;
    std::string gfxStatus;    // the game icons (gfx.h): "" = loaded

private:
    // Home = the welcome page (open an account / attach to a client / a capture), Config = the global
    // settings, Bot = the current session's bot; the rest = the Debug view's pages.
    enum class Tab { Home, Config, Session, World, Character, Packets, Opcodes, Send, Bot, Log };
    static bool debug_page(Tab t) { return t != Tab::Home && t != Tab::Config && t != Tab::Bot; }

    // Packet list filter. Expression tokens (space/comma separated):
    //   24 0x18      only these opcodes        !0x76C   never this opcode
    //   move         name contains "move"      !heart   name does not contain "heart"
    struct Filter {
        int dir = 0;          // 0 all, 1 C2S, 2 S2C
        bool hideReplay = false;
        std::set<uint32_t> incOps, excOps;
        std::vector<std::string> incText, excText;
        uint64_t gen = 1;
    };

    // Rows of the selected session that pass the filter (absolute packet indices).
    struct View {
        uint32_t sessionUid = 0;
        uint64_t filterGen = 0;
        uint64_t scannedUpTo = 0;
        std::vector<uint64_t> rows;
    };

    void load_config();
    void load_db();
    void start_launch(const Account& acc);
    void start_attach(uint32_t pid);
    void open_capture(const std::string& path);
    Session* current();
    Session* find_session(uint32_t uid);

    // the VS Code frame: activity bar | side bar | editor (tabs, page, bottom panel) / status bar
    void draw_activity_bar(float h);
    void draw_side_bar(float h);
    void draw_editor(float h);
    void draw_panel(float h);
    void draw_status_bar(float h);
    void draw_home();
    void draw_config();
    void draw_session();             // Debug > Session: the links, hook state, unlink / kill
    void draw_packets();
    void draw_packet_detail(Session& s, const CapPacket& p);
    void draw_hex(const uint8_t* p, size_t n);
    void draw_opcodes();
    void draw_send();
    void draw_log();
    void draw_bot();                 // neo_app_bot.cpp: the bot's page of the current tab
    void draw_bot_toolbar(Session& s);
    void bot_overview(Session& s);
    void bot_heal_buffs(Session& s);  // Heal & Buffs: the two actions on one page
    void bot_heal(Session& s);
    void bot_buffs(Session& s);
    void bot_loot(Session& s);
    void bot_attack(Session& s);
    void bot_drill(Session& s);      // neo_app_drill.cpp
    void bot_quest(Session& s);      // neo_app_quest.cpp
    void bot_keyquest(Session& s);   // neo_app_keyquest.cpp
    void bot_story(Session& s);      // neo_app_story.cpp
    // The Key Quest's / the Story's activity panel: what it is doing right now, the step, the progress, how long since
    // the last request, and why it stands still (neo_app_keyquest.cpp)
    void quest_activity(Session& s, QuestRunner& qr, const char* what);
    // the panel itself (the Monster Quest's too): the state pill, its line, the step, the progress, the last request
    struct Activity {
        int tone = 0;                   // 0 muted, 1 ok, 2 warn, 3 error, 4 accent
        std::string state, line, step, progress;
        float frac = 0.0f;
        bool showLog = false;           // the latest log lines under it
    };
    void activity_box(Session& s, BotAction& a, const char* what, const Activity& act);
    void monq_activity(Session& s);  // neo_app_quest.cpp
    void bot_hunt(Session& s);       // neo_app_hunt.cpp
    void bot_roam_travel(Session& s);  // Roam & Travel: the Roam, the Town (death, the way back), the Travel
    void bot_roam(Session& s);
    void bot_town(Session& s);       // neo_app_town.cpp: death, recovery, the way back
    void bot_supply(Session& s);     //   Auto-Supply: the shop, the refill, when a trip runs
    void bot_sell(Session& s);       //   Auto-Sell
    void bot_storage(Session& s);    //   Auto-Storage
    void bot_travel(Session& s);     //   the map graph, a trip to a map
    // Auto-Sell / Auto-Storage: the bag's items (and the kept ones) with a "keep" checkbox each (f.black = kept), what
    // each would do now, and a search to keep an item that is not in the bag; kind 2 = sell, 3 = store
    void draw_keep_list(Session& s, int kind, IdFilter& f);
    // a map picker over the atlas (name / id search); true = changed
    bool map_picker(const char* id, int& zone, std::string& query, float width);
    // an NPC picker: shops (bank false, shops true) or warehouse keepers; true = changed
    bool npc_picker(const char* id, std::string& key, bool shops, std::string& query, float width);
    std::string townMapQuery_, travelMapQuery_, shopQuery_, whQuery_;
    int         travelZone_ = 0;
    uint32_t    buyAdd_ = 0;
    std::vector<Route> travelPreview_;         // the Travel tab's routes: every way out, the quickest first
    std::string travelPreviewKey_;             // what it was planned for
    std::string bot_profile_path(const Session& s) const;
    void bot_load_profile(Session& s, bool quiet);
    void bot_save_profile(Session& s);
    // the Key Quest's book (what the server told of each quest's history): its own file per character, written
    // whenever it changes (profiles\quests_<char uid>.txt)
    std::string bot_book_path(const Session& s) const;
    void bot_load_book(Session& s);
    void bot_save_book(Session& s);
    // a black / whitelist editor: kind 0 = items (Loot), 1 = monster species (Attack), 2 / 3 = items
    // (Supply: sell / store); `nearby` = quick adds
    void draw_id_list(int kind, IdFilter& f, const std::vector<uint32_t>& nearby);
    std::string listQuery_[4], listQueryFor_[4];
    std::vector<uint32_t> listResults_[4];
    std::string botStatus_;
    enum BotTab { BT_Overview, BT_HealBuffs, BT_Loot, BT_Attack, BT_Drill, BT_Quest, BT_KeyQuest, BT_Story, BT_Hunt, BT_Travel,
                  BT_Supply, BT_Sell, BT_Storage, BT_Count };
    static int bot_tab_of(const char* actionKey);    // an action's page
    int         botTab_ = BT_Overview;
    int         pageScroll_ = -1;          // --scroll <px>: the page scrolled that far (UI checks with --shot)
    void draw_world();               // neo_app_world.cpp
    void draw_character();
    // Right-click on the radar -> true with the world point under the mouse.
    // With a bot: its start point, roam destination and target are drawn too.
    bool draw_radar(const WorldModel& w, float size, int& clickX, int& clickY, const Bot* bot = nullptr);

    void compile_filter();
    bool filter_match(const CapPacket& p) const;
    bool update_view(Session& s);     // true when rows were added
    std::string packet_name(uint8_t dir, uint16_t op) const;
    void load_into_editor(const CapPacket& p);
    void export_view(Session& s);

    Config      cfg_;
    std::string cfgError_;
    PacketDb    db_;
    std::string dbStatus_;
    GameData    gameData_;
    std::string gameDataStatus_;

    std::vector<std::unique_ptr<Session>> sessions_;
    uint32_t nextUid_ = 1;
    uint32_t curUid_ = 0;
    Tab      tab_ = Tab::Home;
    Tab      lastDebug_ = Tab::World;
    bool     sideOpen_ = true;              // the side bar (a click on the active activity icon hides it)
    bool     secChar_ = true, secEffects_ = true;
    Config   cfgEdit_;                      // the Settings page's copy
    // A bot ended / stopped (not by you): a notice in the window's corner (until clicked, or 15 s once the window is
    // in front) + a Windows notification and the taskbar flashing (notify.h). Bot::stopCount seen per session.
    struct Toast { std::string title, text; bool warning = false; double until = 0.0; };
    std::vector<Toast> toasts_;
    std::map<const Session*, int> stopSeen_;
    void notice_stops(Session& s);
    void draw_toasts();
    bool     cfgEditLoaded_ = false;
    std::string cfgStatus_;
    enum PanelTab { PT_Activity, PT_Events, PT_Output };
    int      panelTab_ = PT_Activity;
    bool     panelOpen_ = true;
    bool     panelMin_ = false;             // only its header (the minimize button)
    float    panelH_ = 210.0f;
    uint64_t panelSeen_ = 0;                // what the panel list held last frame (follow new lines)

    // Clients
    int                    accountIdx_ = 0;
    std::vector<ProcEntry> procs_;
    double                 procsAt_ = -100.0;
    std::string            capturePath_;

    // Packets
    Filter      filter_;
    std::string filterExpr_;
    View        view_;
    bool        follow_ = true;
    bool        paused_ = false;
    uint64_t    selected_ = UINT64_MAX;     // absolute index in the current session
    std::string exportStatus_;

    // Send
    std::string sendOp_;
    int         sendParam_ = 0;
    std::string sendHex_;
    uint32_t    sendCtx_ = 0;               // 0 = auto
    std::string sendStatus_;
    bool        sendStatusOk_ = false;
    bool        editorFromS2C_ = false;
    bool        sendFieldMode_ = true;              // field editor vs raw hex
    std::vector<std::string> sendFieldVals_;        // one per field of the current opcode
    int         sendFieldsFor_ = -1;                // opcode the vals belong to (re-init on change)

    void set_editor(uint16_t op, const std::vector<uint8_t>& body, uint32_t ctx);  // quick actions
    void draw_quick_actions(Session& s);
    // A quick action: into the editor, and sent at once when quickSend_ is on.
    void do_action(Session& s, const Action& a);

    // Quick actions
    bool        quickSend_ = false;         // send the action at once (else only fill the editor)
    bool        quickRun_ = true;           // move type 2 run / 1 walk
    int         skillSel_ = 0;              // skill id
    int         skillTargetMode_ = 0;       // SkillTarget::Mode
    int         skillTargetEid_ = 0;
    uint64_t    itemSel_ = 0;               // bag uid
    int         moveX_ = 0, moveY_ = 0;
    bool        moveSet_ = false;           // moveX_/moveY_ hold a point (radar right-click / input)
    std::string actionStatus_;
    bool        actionStatusOk_ = true;

    // Log
    uint64_t logSeen_ = 0;

    // World
    float radarRange_ = 600.0f;
    bool  showKind_[4] = { true, true, true, true };   // monsters, players, npcs, pets
    bool  hideDead_ = false;
    bool  eventsFollow_ = true;
};

} // namespace nj
