#include "app/neo_app.h"
#include "app/gfx.h"
#include "app/notify.h"
#include "app/text_util.h"
#include "app/ui_theme.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include <algorithm>
#include <utility>
#include <array>
#include <cctype>
#include <fstream>
#include <functional>

namespace nj {

namespace {

const ImVec4 kColC2S      = ImVec4(0.45f, 0.78f, 1.00f, 1.0f);
const ImVec4 kColS2C      = ImVec4(0.55f, 0.92f, 0.55f, 1.0f);
const ImVec4 kColInjected = ImVec4(1.00f, 0.66f, 0.25f, 1.0f);
const ImVec4 kColWarn     = ImVec4(1.00f, 0.80f, 0.30f, 1.0f);
const ImVec4 kColErr      = ImVec4(1.00f, 0.42f, 0.42f, 1.0f);
const ImVec4 kColOk       = ImVec4(0.50f, 0.90f, 0.50f, 1.0f);

std::string lower(std::string s)
{
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

const char* dir_name(uint8_t dir) { return dir == DIR_C2S ? "C>S" : "S>C"; }

const char* hook_state_name(uint32_t s)
{
    switch (s) {
    case HOOK_WAITING: return "waiting for the client code";
    case HOOK_ACTIVE:  return "active";
    case HOOK_FAILED:  return "FAILED";
    }
    return "?";
}

ImVec4 state_color(Session::State s)
{
    switch (s) {
    case Session::State::Live:   return kColOk;
    case Session::State::Failed: return kColErr;
    case Session::State::Exited:
    case Session::State::Closed: return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    default:                     return kColWarn;
    }
}

void help_marker(const char* text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

} // namespace

NeoApp::NeoApp()
{
    load_config();
    load_db();
}

NeoApp::~NeoApp() = default;

void NeoApp::load_config()
{
    cfgError_.clear();
    if (!nj::load_config(cfg_, cfgError_)) app_log("config: %s", cfgError_.c_str());
    else app_log("config: %s", cfg_.iniPath.c_str());
    if (accountIdx_ >= (int)cfg_.accounts.size()) accountIdx_ = 0;
}

void NeoApp::load_db()
{
    db_.clear();
    const std::string dir = exe_dir() + "data\\";
    std::string err;
    dbStatus_.clear();
    for (const char* f : { "packets_db.yml", "packets_neo.yml" }) {
        if (!db_.load(dir + f, err)) {
            app_log("packet db: %s", err.c_str());
            dbStatus_ += err + "  ";
        }
    }
    app_log("packet db: %zu definitions", db_.size());
    filter_.gen++;

    bool haveLib = !cfg_.libConfig.empty() && file_exists(cfg_.libConfig);
    if (cfg_.libConfig.empty()) {                       // the first conf\*.xml next to the exe
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((exe_dir() + "conf\\*.xml").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) { haveLib = true; FindClose(h); }
    }
    if (cfg_.gameDbDir.empty() && !haveLib) {
        gameDataStatus_ = "no game data: set your server's libconfig XML and data folder (Settings > Data)";
    } else {
        std::string gerr;
        gameData_.set_map_dir(cfg_.mapDir);
        gameData_.set_libconfig(cfg_.libConfig);
        gameData_.set_data_dir(cfg_.dataDir);
        gameData_.load(cfg_.gameDbDir, gerr);
        gameDataStatus_ = fmt("game data (%s): %zu items, %zu monsters, %zu skills, %zu maps", gameData_.source().c_str(),
                              gameData_.items(), gameData_.mobs(), gameData_.skills(), gameData_.maps());
        if (!gerr.empty()) gameDataStatus_ += "  (" + gerr + ")";
    }
    app_log("%s", gameDataStatus_.c_str());
}

void NeoApp::start_launch(const Account& acc)
{
    auto s = std::make_unique<Session>(nextUid_++, &gameData_);
    s->launch(cfg_, acc);
    curUid_ = s->uid;
    selected_ = UINT64_MAX;
    sessions_.push_back(std::move(s));
    tab_ = Tab::Bot;
}

void NeoApp::start_attach(uint32_t pid)
{
    auto s = std::make_unique<Session>(nextUid_++, &gameData_);
    s->attach(cfg_, pid);
    curUid_ = s->uid;
    selected_ = UINT64_MAX;
    sessions_.push_back(std::move(s));
    tab_ = Tab::Bot;
}

void NeoApp::handle_command_line(const char* cmdLine)
{
    std::vector<std::string> args;
    std::string cur;
    bool quoted = false;
    for (const char* c = cmdLine; c && *c; ++c) {
        if (*c == '"') { quoted = !quoted; continue; }
        if (*c == ' ' && !quoted) {
            if (!cur.empty()) args.push_back(cur);
            cur.clear();
            continue;
        }
        cur += *c;
    }
    if (!cur.empty()) args.push_back(cur);

    for (size_t i = 0; i + 1 < args.size(); ++i) {
        const std::string& a = args[i];
        const std::string& v = args[i + 1];
        if (a == "--launch") {
            Account acc{ v, v };
            for (const Account& x : cfg_.accounts)
                if (x.login == v) acc = x;
            start_launch(acc);
            tab_ = Tab::Bot;
            ++i;
        } else if (a == "--attach") {
            uint32_t pid = 0;
            if (parse_uint(v, pid)) start_attach(pid);
            tab_ = Tab::Bot;
            ++i;
        } else if (a == "--open") {
            open_capture(v);
            ++i;
        } else if (a == "--toast") {                // (UI check: two sample notices in the window, no Windows one)
            toasts_.push_back(Toast{ "Leao: done", "item hunt done: the 4 item(s) of the list are in the bag (61 picked up "
                                     "in 1.3 h)", false, 0.0 });
            toasts_.push_back(Toast{ "Leao: the bot stopped", "you died", true, 0.0 });
        } else if (a == "--scroll") {
            uint32_t px = 0;
            if (parse_uint(v, px)) pageScroll_ = int(px);
            ++i;
        } else if (a == "--tab") {
            const std::string t = lower(v);
            static const char* kBot[BT_Count] = { "overview", "heal", "loot", "attack", "drill", "quest", "keyquest", "story", "hunt",
                                                  "travel", "supply", "sell", "storage" };
            for (int b = 0; b < BT_Count; ++b)
                if (t == kBot[b]) { botTab_ = b; tab_ = Tab::Bot; }
            if (std::find(std::begin(kBot), std::end(kBot), t) == std::end(kBot))
                tab_ = t == "world" ? Tab::World : t == "character" ? Tab::Character
                     : t == "packets" ? Tab::Packets : t == "opcodes" ? Tab::Opcodes : t == "send" ? Tab::Send : t == "bot" ? Tab::Bot
                     : t == "log" ? Tab::Log : t == "session" ? Tab::Session : t == "settings" ? Tab::Config : Tab::Home;
            ++i;
        }
    }
}

void NeoApp::open_capture(const std::string& path)
{
    auto s = std::make_unique<Session>(nextUid_++, &gameData_);
    std::string err;
    if (!s->load_capture(path, err)) {
        app_log("open capture: %s", err.c_str());
        return;
    }
    curUid_ = s->uid;
    selected_ = UINT64_MAX;
    sessions_.push_back(std::move(s));
    tab_ = Tab::Bot;
}

Session* NeoApp::find_session(uint32_t uid)
{
    for (auto& s : sessions_)
        if (s->uid == uid) return s.get();
    return nullptr;
}

Session* NeoApp::current() { return find_session(curUid_); }

std::string NeoApp::packet_name(uint8_t dir, uint16_t op) const
{
    const PacketDef* d = db_.find(dir, op);
    return d ? d->name : std::string();
}

// ── frame ───────────────────────────────────────────────────────────────────
void NeoApp::update()
{
    const uint64_t now = now_filetime();
    for (auto& s : sessions_) {
        s->poll();
        notice_stops(*s);
        Bot& bot = s->bot;
        if (s->botProfileFor && s->bookSaved != bot.book.version()) bot_save_book(*s);
        if (bot.notes != s->logWritten) {               // the bot's log, appended to logs\bot_<char uid>.log
            const uint64_t fresh = std::min<uint64_t>(bot.notes - s->logWritten, bot.log.size());
            const uint64_t uid = s->world ? s->world->self.charUid : 0;
            if (uid) {
                CreateDirectoryA((exe_dir() + "logs").c_str(), nullptr);
                std::ofstream f(exe_dir() + fmt("logs\\bot_%016llX.log", (unsigned long long)uid), std::ios::binary | std::ios::app);
                if (!s->logWritten) f << "=== " << filetime_local_date(now) << " " << s->world->self.name << "\r\n";
                for (size_t i = bot.log.size() - size_t(fresh); i < bot.log.size(); ++i) f << bot.log[i] << "\r\n";
            }
            s->logWritten = bot.notes;
        }
        if (!bot.running) continue;
        if (s->finished()) { bot.stop(fmt("session %s", s->state_name())); continue; }
        if (s->state != Session::State::Live) { bot.doing = "waiting for the link"; continue; }
        Action a;
        if (!bot.tick(*s->world, now, a)) continue;
        std::string err;
        if (!s->send(s->gameCtx, a.op, 0, a.body, err)) bot.stop("send failed: " + err);
    }
}

// A new stop of this session's bot: the notices (not for a stop you made).
void NeoApp::notice_stops(Session& s)
{
    const Bot& bot = s.bot;
    int& seen = stopSeen_[&s];
    if (bot.stopCount == seen) return;
    const bool fresh = bot.stopCount > seen;
    seen = bot.stopCount;
    if (!fresh || bot.stopByUser || !cfg_.notifyStop) return;
    const std::string who = s.world && !s.world->self.name.empty() ? s.world->self.name : s.label;
    const std::string title = who + (bot.stopFinished ? ": done" : ": the bot stopped");
    notify::show("Trickster Automation Studio - " + title, bot.stopWhy, !bot.stopFinished);
    toasts_.push_back(Toast{ title, bot.stopWhy, !bot.stopFinished, 0.0 });
}

// The notices, newest at the bottom right over the status bar.
void NeoApp::draw_toasts()
{
    const double t = ImGui::GetTime();
    const ImGuiIO& io = ImGui::GetIO();
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [&](const Toast& x) { return x.until > 0.0 && t > x.until; }),
                  toasts_.end());
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float y = vp->WorkPos.y + vp->WorkSize.y - 34.0f;
    for (size_t i = toasts_.size(); i-- > 0;) {
        Toast& x = toasts_[i];
        if (x.until == 0.0 && !io.AppFocusLost) x.until = t + 15.0;    // (seen: 15 s from now)
        const ImVec4 col = x.warning ? ui::kWarn : ui::kOk;
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 16.0f, y), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(440, 0), ImVec2(440, 400));
        ImGui::SetNextWindowBgAlpha(0.98f);
        ImGui::PushStyleColor(ImGuiCol_Border, col);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 10));
        ImGui::Begin(fmt("##toast%zu", i).c_str(), nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
        ImGui::TextColored(col, "%s", x.title.c_str());
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 410.0f);
        ImGui::TextUnformatted(x.text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::TextColored(ui::kMuted, "click to close");
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0)) x.until = t - 1.0;
        y -= ImGui::GetWindowHeight() + 8.0f;
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }
}

void NeoApp::frame()
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::Begin("Trickster Automation Studio", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    const float statusH = 22.0f;
    const float bodyH = std::max(50.0f, ImGui::GetContentRegionAvail().y - statusH);
    if (!current() && (tab_ == Tab::Bot)) tab_ = Tab::Home;

    draw_activity_bar(bodyH);
    ImGui::SameLine(0, 0);
    if (sideOpen_ && current() && (tab_ == Tab::Bot || debug_page(tab_))) {
        draw_side_bar(bodyH);
        ImGui::SameLine(0, 0);
    }
    draw_editor(bodyH);
    ImGui::SetCursorPos(ImVec2(0, bodyH));
    draw_status_bar(statusH);
    ImGui::PopStyleVar(2);
    ImGui::End();
    draw_toasts();
}

namespace {
// the first n characters (code points) of a UTF-8 string
std::string utf8_prefix(const std::string& s, size_t n)
{
    size_t i = 0, k = 0;
    while (i < s.size() && k < n) {
        const unsigned char c = (unsigned char)s[i];
        i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        ++k;
    }
    return s.substr(0, std::min(i, s.size()));
}
} // namespace

// ── activity bar: one bot per session, Debug, Settings, + (Home) ────────────
void NeoApp::draw_activity_bar(float h)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::kSurface);
    ImGui::BeginChild("activity", ImVec2(48, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    uint32_t removeUid = 0;
    for (auto& sp : sessions_) {
        Session& s = *sp;
        const SelfState& me = s.world->self;
        const bool active = tab_ == Tab::Bot && s.uid == curUid_;
        const ImVec4 badge = s.bot.running ? ui::kStatusRun : s.state == Session::State::Live ? ui::kOk
                           : s.finished() ? ui::kMuted : ui::kWarn;
        const std::string name = me.name.empty() ? s.label : me.name;
        const std::string tip = fmt("%s\n%s  -  %s%s\nright-click: unlink / kill / close", name.c_str(), s.label.c_str(),
                                    s.state_name(), s.bot.running ? "  -  bot running" : "");
        ImGui::PushID((int)s.uid);
        if (ui::activity_button("##ses", ui::Glyph::Bot, active, tip.c_str(), utf8_prefix(name, 6).c_str(), &badge)) {
            if (active) sideOpen_ = !sideOpen_;
            else {
                curUid_ = s.uid;
                selected_ = UINT64_MAX;
                tab_ = Tab::Bot;
                sideOpen_ = true;
            }
        }
        if (ImGui::BeginPopupContextItem("##ctx")) {
            ImGui::TextDisabled("%s", name.c_str());
            ImGui::Separator();
            if (!s.finished()) {
                if (ImGui::MenuItem("Unlink")) s.close();
                ImGui::SetItemTooltip("Drop the link. The DLL stays in the client and keeps capturing; attach again to resume.");
                if (ImGui::MenuItem("Kill the client")) s.kill_client();
            }
            if (ImGui::MenuItem("Close the session")) removeUid = s.uid;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (removeUid) {
        sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                       [&](const std::unique_ptr<Session>& s) { return s->uid == removeUid; }),
                        sessions_.end());
        if (curUid_ == removeUid) curUid_ = sessions_.empty() ? 0 : sessions_.back()->uid;
        if (sessions_.empty()) tab_ = Tab::Home;
    }

    const bool dbg = debug_page(tab_);
    if (ui::activity_button("##dbg", ui::Glyph::Debug, dbg, "Debug  -  session, world, character, packets, send, log")) {
        if (dbg) sideOpen_ = !sideOpen_;
        else { tab_ = lastDebug_; sideOpen_ = true; }
    }
    if (ui::activity_button("##cfg", ui::Glyph::Gear, tab_ == Tab::Config, "Settings  -  Trickster Automation Studio (tas.ini)"))
        tab_ = Tab::Config;
    if (ui::activity_button("##new", ui::Glyph::Plus, tab_ == Tab::Home, "New session  -  open an account or attach to a client"))
        tab_ = Tab::Home;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    dl->AddLine(ImVec2(wp.x + 47.5f, wp.y), ImVec2(wp.x + 47.5f, wp.y + h), ui::col(ui::kBorder));
    ImGui::EndChild();
}

// ── side bar: the character, the effects on it ──────────────────────────────
void NeoApp::draw_side_bar(float h)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::kSurface);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 4));
    ImGui::BeginChild("sidebar", ImVec2(270, h), ImGuiChildFlags_None);
    ImGui::PopStyleColor();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    dl->AddLine(ImVec2(wp.x + 269.5f, wp.y), ImVec2(wp.x + 269.5f, wp.y + h), ui::col(ui::kBorder));
    const uint64_t now = now_filetime();
    Session* cur = current();

    // title row, as VS Code's "EXPLORER": the view + the session
    ImGui::SetCursorPos(ImVec2(20, 10));
    ImGui::PushFont(ui::fonts.small);
    ImGui::TextColored(ui::kMuted, "%s  -  %s", tab_ == Tab::Bot ? "BOT" : "DEBUG", cur ? cur->label.c_str() : "");
    ImGui::PopFont();
    ImGui::SetCursorPosY(35);

    if (cur && ui::section("Character", &secChar_)) {
        const WorldModel& w = *cur->world;
        const SelfState& me = w.self;
        ImGui::Indent(20);
        const float bw = ImGui::GetContentRegionAvail().x - 14;
        ImGui::PushFont(ui::fonts.bold);
        ImGui::TextUnformatted(me.name.empty() ? cur->state_name() : me.name.c_str());
        ImGui::PopFont();
        if (!me.name.empty()) {
            ui::label(fmt("Lv %d  -  TM %d  -  %s", me.level, me.tmLevel, gameData_.map_name(me.zone).c_str()).c_str());
            ui::bar(me.hpMax > 0 ? float(me.hp) / float(me.hpMax) : 0.0f, ImVec4(0.75f, 0.22f, 0.25f, 1), bw,
                    fmt("HP  %lld / %lld", (long long)me.hp, (long long)me.hpMax).c_str());
            ui::bar(me.mpMax > 0 ? float(me.mp) / float(me.mpMax) : 0.0f, ImVec4(0.18f, 0.42f, 0.85f, 1), bw,
                    fmt("MP  %lld / %lld", (long long)me.mp, (long long)me.mpMax).c_str());
            ui::label(fmt("galder %lld  -  EXP +%lld this session", (long long)me.money, (long long)me.expGained).c_str());
        }
        ImGui::Unindent(20);
        ImGui::Dummy(ImVec2(0, 6));
    }

    if (cur && cur->world->inWorld && ui::section("Effects", &secEffects_)) {
        const WorldModel& w = *cur->world;
        ImGui::Indent(20);
        int n = 0;
        const float size = 30.0f;
        const float avail = ImGui::GetContentRegionAvail().x - 10;
        const int perRow = std::max(1, int(avail / (size + 6)));
        for (const Effect& e : w.effects) {
            if (!WorldModel::effect_up(e, now)) continue;
            if (n % perRow) ImGui::SameLine(0, 6);
            const float left = e.endAt ? float(double(e.endAt > now ? e.endAt - now : 0) / 1e7) : -1.0f;
            const float life = e.endAt ? float(double(e.endAt - e.startAt) / 1e7) : 0.0f;
            const float spent = life > 0 ? 1.0f - left / life : 0.0f;
            ImGui::PushID(n);
            ui::icon_cool(gfx::buff(e.skill), size, e.ending ? std::max(spent, 0.01f) : 0.0f,
                          left >= 0 && left < 60 ? fmt("%.0f", left).c_str() : "");
            ImGui::PopID();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s%s\n%s", gameData_.skill_name(e.skill).c_str(),
                                  e.caster != w.self.eid ? fmt("  (from %s)", w.entity_label(e.caster).c_str()).c_str() : "",
                                  left >= 0 ? fmt("%d:%02d left", int(left) / 60, int(left) % 60).c_str() : "lasts until it is ended");
            ++n;
        }
        if (!n) ui::label("none");
        int cn = 0;                             // our skills still cooling
        for (const SkillEntry& sk : w.skills) {
            const uint64_t ready = w.skill_ready_at(sk.id);
            if (!ready || ready <= now) continue;
            if (!cn) { ImGui::Dummy(ImVec2(0, 4)); ui::label("cooling"); }
            if (cn % perRow) ImGui::SameLine(0, 6);
            const SkillLevelInfo* li = gameData_.skill_level(sk.id, sk.level);
            const double total = li ? std::max(0.5, double(li->coolTime) - 0.5) : 1.0;
            const double left = double(ready - now) / 1e7;
            ImGui::PushID(1000 + cn);
            ui::icon_cool(gfx::skill(sk.id), size, float(left / total), fmt("%.0f", std::ceil(left)).c_str());
            ImGui::PopID();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s  -  %.1f s", gameData_.skill_name(sk.id).c_str(), left);
            ++cn;
        }
        ImGui::Unindent(20);
    }

    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY() + 8, h - 26));
    ImGui::SetCursorPosX(20);
    ui::label(gfxStatus.empty() ? fmt("%zu icons  -  %zu packet defs", gfx::count(), db_.size()).c_str() : gfxStatus.c_str());
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

// ── the editor group: tabs, the page, the bottom panel ──────────────────────
void NeoApp::draw_editor(float h)
{
    ImGui::BeginChild("editor", ImVec2(0, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    Session* s = current();
    if (tab_ == Tab::Bot) {
        static const char* kTabs[BT_Count] = { "Overview", "Heal & Buffs", "Loot", "Attack", "Drill", "Monster Quest", "Key Quest", "Story Quest",
                                               "Item Hunt", "Roam & Travel", "Auto-Supply", "Auto-Sell", "Auto-Storage" };
        bool dots[BT_Count] = {};
        if (s) {
            dots[BT_Overview] = s->bot.running;
            for (const BotAction* a : s->bot.actions()) dots[bot_tab_of(a->key())] |= a->enabled;
            dots[BT_Travel] = s->bot.roam.enabled || s->bot.town.enabled || s->bot.town.trip();
            dots[BT_Supply] = s->bot.town.supply;
            dots[BT_Sell] = s->bot.town.sell;
            dots[BT_Storage] = s->bot.town.store;
        }
        ui::tab_strip("bottabs", &botTab_, kTabs, BT_Count, s ? dots : nullptr);
    } else if (tab_ == Tab::Home || tab_ == Tab::Config) {
        const char* one[] = { tab_ == Tab::Home ? "Welcome" : "Settings" };
        int z = 0;
        ui::tab_strip("onetab", &z, one, 1);
    } else {
        static const char* kTabs[] = { "Session", "World", "Character", "Packets", "Opcodes", "Send", "Log" };
        static const Tab kIds[] = { Tab::Session, Tab::World, Tab::Character, Tab::Packets, Tab::Opcodes, Tab::Send, Tab::Log };
        int sel = 0;
        for (int i = 0; i < 7; ++i) if (kIds[i] == tab_) sel = i;
        if (ui::tab_strip("dbgtabs", &sel, kTabs, 7)) tab_ = kIds[sel];
        lastDebug_ = tab_;
    }

    const float splitter = 5.0f, header = 32.0f;
    const float maxPanel = std::max(80.0f, h - 200.0f);
    const float panelH = !panelOpen_ ? 0.0f : panelMin_ ? header : std::clamp(panelH_, 80.0f, maxPanel);
    const float pageH = std::max(60.0f, ImGui::GetContentRegionAvail().y - panelH - (panelOpen_ ? splitter : 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 14));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));
    ImGui::BeginChild("page", ImVec2(0, pageH), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (pageScroll_ >= 0) ImGui::SetScrollY(float(pageScroll_));
    switch (tab_) {
    case Tab::Home:      draw_home(); break;
    case Tab::Config:    draw_config(); break;
    case Tab::Session:   draw_session(); break;
    case Tab::World:     draw_world(); break;
    case Tab::Character: draw_character(); break;
    case Tab::Packets:   draw_packets(); break;
    case Tab::Opcodes:   draw_opcodes(); break;
    case Tab::Send:      draw_send(); break;
    case Tab::Bot:       draw_bot(); break;
    case Tab::Log:       draw_log(); break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);

    if (panelOpen_) {
        // the splitter: drag to resize the panel (not while it is minimized)
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("##split", ImVec2(w, splitter));
        const bool drag = !panelMin_ && (ImGui::IsItemHovered() || ImGui::IsItemActive());
        if (!panelMin_ && ImGui::IsItemActive()) panelH_ = std::clamp(panelH_ - ImGui::GetIO().MouseDelta.y, 80.0f, maxPanel);
        if (drag) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + splitter - 0.5f), ImVec2(p.x + w, p.y + splitter - 0.5f),
                                            ui::col(drag ? ui::kAccent : ui::kBorder), drag ? 2.0f : 1.0f);
        draw_panel(ImGui::GetContentRegionAvail().y);
    }
    ImGui::EndChild();
}

// ── the bottom panel: the bot's activity, the world's events, the app's output ──
void NeoApp::draw_panel(float h)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::kSurface);
    ImGui::BeginChild("panel", ImVec2(0, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    Session* s = current();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // header: ACTIVITY  EVENTS  OUTPUT ......... [minimize] [x]
    static const char* kTabs[] = { "ACTIVITY", "EVENTS", "OUTPUT" };
    ImGui::SetCursorPos(ImVec2(12, 0));
    for (int i = 0; i < 3; ++i) {
        ImGui::PushFont(ui::fonts.small);
        const ImVec2 ts = ImGui::CalcTextSize(kTabs[i]);
        ImGui::PopFont();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##pt", ImVec2(ts.x + 20, 32))) { panelTab_ = i; panelMin_ = false; }
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool on = panelTab_ == i;
        dl->AddText(ui::fonts.small, ui::fonts.small->FontSize, ImVec2(p.x + 10, p.y + (32 - ts.y) * 0.5f),
                    ui::col(on || hov ? ImVec4(0.91f, 0.91f, 0.91f, 1) : ui::kMuted), kTabs[i]);
        if (on && !panelMin_) dl->AddRectFilled(ImVec2(p.x + 10, p.y + 30), ImVec2(p.x + 10 + ts.x, p.y + 31), ui::col(ui::kAccent));
        ImGui::SameLine(0, 0);
    }
    // the header buttons, VS Code's panel actions: minimize (a chevron) and close (an x)
    auto action = [&](const char* id, float x, const char* tip) {
        ImGui::SetCursorPos(ImVec2(x, 4));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool click = ImGui::InvisibleButton(id, ImVec2(24, 24));
        if (ImGui::IsItemHovered()) { dl->AddRectFilled(p, ImVec2(p.x + 24, p.y + 24), ui::col(ui::kHover), 4.0f); ImGui::SetTooltip("%s", tip); }
        return std::make_pair(click, p);
    };
    const float right = ImGui::GetWindowWidth() - 12;
    const ImU32 ic = ui::col(ui::kText);
    {
        auto [click, p] = action("##min", right - 52, panelMin_ ? "Restore the panel" : "Minimize the panel");
        if (click) panelMin_ = !panelMin_;
        const float cy = p.y + 12, cx = p.x + 12;
        if (panelMin_) dl->AddPolyline(std::array<ImVec2, 3>{ ImVec2(cx - 5, cy + 2.5f), ImVec2(cx, cy - 2.5f), ImVec2(cx + 5, cy + 2.5f) }.data(), 3, ic, 0, 1.4f);
        else dl->AddPolyline(std::array<ImVec2, 3>{ ImVec2(cx - 5, cy - 2.5f), ImVec2(cx, cy + 2.5f), ImVec2(cx + 5, cy - 2.5f) }.data(), 3, ic, 0, 1.4f);
    }
    {
        auto [click, p] = action("##close", right - 24, "Hide the panel (the status bar brings it back)");
        if (click) panelOpen_ = false;
        dl->AddLine(ImVec2(p.x + 8, p.y + 8), ImVec2(p.x + 16, p.y + 16), ic, 1.4f);
        dl->AddLine(ImVec2(p.x + 16, p.y + 8), ImVec2(p.x + 8, p.y + 16), ic, 1.4f);
    }
    if (panelMin_) { ImGui::EndChild(); return; }

    ImGui::SetCursorPos(ImVec2(0, 32));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 1));
    ImGui::BeginChild("panelbody", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(ui::fonts.mono);
    const bool atEnd = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4;
    uint64_t version = 0;
    if (panelTab_ == PT_Activity) {
        if (!s) ImGui::TextColored(ui::kMuted, "no client");
        else {
            for (const std::string& line : s->bot.log) {
                const bool bad = line.find("refused") != std::string::npos || line.find("stopped") != std::string::npos ||
                                 line.find("off for") != std::string::npos || line.find("no answer") != std::string::npos;
                const bool warn = line.find(" left") != std::string::npos || line.find(" no ") != std::string::npos ||
                                  line.find("cancelled") != std::string::npos;
                ImGui::TextColored(bad ? ui::kErr : warn ? ui::kWarn : ui::kText, "%s", line.c_str());
            }
            version = s->bot.log.size() * 1000003ull + (s->bot.log.empty() ? 0 : std::hash<std::string>{}(s->bot.log.back()));
        }
    } else if (panelTab_ == PT_Events) {
        if (!s) ImGui::TextColored(ui::kMuted, "no client");
        else {
            ImGuiListClipper clip;
            clip.Begin(int(s->world->events.size()));
            while (clip.Step())
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) ImGui::TextUnformatted(s->world->events[size_t(i)].c_str());
            version = s->world->events.size() * 1000003ull +
                      (s->world->events.empty() ? 0 : std::hash<std::string>{}(s->world->events.back()));
        }
    } else {
        app_log_read([&](const std::deque<std::string>& lines) {
            ImGuiListClipper clip;
            clip.Begin((int)lines.size());
            while (clip.Step())
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) ImGui::TextUnformatted(lines[size_t(i)].c_str());
        });
        version = app_log_version();
    }
    if (version != panelSeen_ && atEnd) ImGui::SetScrollHereY(1.0f);
    panelSeen_ = version;
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::EndChild();
}

// ── status bar ──────────────────────────────────────────────────────────────
void NeoApp::draw_status_bar(float h)
{
    Session* s = current();
    const bool live = s && s->state == Session::State::Live;
    const ImVec4 bg = !s ? ui::kStatusIdle : s->bot.running ? ui::kStatusRun : ui::kStatusLive;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::BeginChild("status", ImVec2(0, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    const uint64_t now = now_filetime();
    ImGui::SetCursorPos(ImVec2(4, 0));
    if (!s) {
        if (ui::status_item("No client  -  open one from the Welcome page", "+ in the activity bar")) tab_ = Tab::Home;
    } else {
        const SelfState& me = s->world->self;
        if (ui::status_item(fmt("%s  %s", s->label.c_str(), live ? "" : s->state_name()).c_str(), "Debug: Session"))
            tab_ = Tab::Session;
        if (!me.name.empty()) {
            ui::status_item(fmt("%s  Lv %d", me.name.c_str(), me.level).c_str());
            int x = 0, y = 0;
            s->world->self_pos_at(now, x, y);
            if (ui::status_item(fmt("%s  (%d, %d)", gameData_.map_name(me.zone).c_str(), x, y).c_str(), "Debug: World"))
                tab_ = Tab::World;
            ui::status_item(fmt("HP %lld/%lld   MP %lld/%lld", (long long)me.hp, (long long)me.hpMax, (long long)me.mp,
                                (long long)me.mpMax).c_str());
        }
    }
    std::vector<std::string> right;
    if (s) {
        right.push_back(s->bot.running ? fmt("Stop bot  -  %s", s->bot.doing.c_str()) : "Start bot");
        right.push_back(fmt("kills %d", s->bot.attack.kills));
    }
    right.push_back(!panelOpen_ ? "Panel (hidden)" : panelMin_ ? "Panel (minimized)" : "Panel");
    right.push_back(fmt("%.0f fps", ImGui::GetIO().Framerate));
    ImGui::PushFont(ui::fonts.small);
    float rw = 0;
    for (auto& t : right) rw += ImGui::CalcTextSize(t.c_str()).x + 16.0f;
    ImGui::PopFont();
    const float rx = ImGui::GetWindowWidth() - rw - 8;
    if (ImGui::GetCursorPosX() < rx) ImGui::SetCursorPosX(rx);
    for (size_t i = 0; i < right.size(); ++i) {
        if (!ui::status_item(right[i].c_str())) continue;
        if (s && i == 0) {
            if (s->bot.running) s->bot.stop_by_user("by you");
            else if (live && s->world->inWorld && !s->world->self.dead) s->bot.start(*s->world, now);
        } else if (right[i].rfind("Panel", 0) == 0) {
            if (!panelOpen_) { panelOpen_ = true; panelMin_ = false; }
            else if (panelMin_) panelMin_ = false;
            else panelOpen_ = false;
        }
    }
    ImGui::EndChild();
}

// ── Welcome: open an account, attach to a running client, replay a capture ──
void NeoApp::draw_home()
{
    const float w = std::min(ImGui::GetContentRegionAvail().x, 1100.0f);
    ImGui::Dummy(ImVec2(0, 26));
    ImGui::PushFont(ui::fonts.title);
    ImGui::TextUnformatted("Trickster Automation Studio");
    ImGui::PopFont();
    ImGui::PushFont(ui::fonts.big);
    ImGui::TextColored(ui::kMuted, "automated test clients for Trickster Online servers");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 18));
    if (!cfgError_.empty()) { ImGui::TextColored(ui::kWarn, "settings: %s", cfgError_.c_str()); ImGui::SameLine(); if (ui::link("open Settings")) tab_ = Tab::Config; }
    if (!file_exists(cfg_.dllPath)) ImGui::TextColored(ui::kErr, "hook DLL missing: %s", cfg_.dllPath.c_str());

    auto heading = [](const char* t) {
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::PushFont(ui::fonts.big);
        ImGui::TextUnformatted(t);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 2));
    };
    if (ImGui::GetTime() - procsAt_ > 2.0) {
        procs_ = list_client_processes(cfg_);
        procsAt_ = ImGui::GetTime();
    }

    if (ImGui::BeginTable("home", 2, ImGuiTableFlags_SizingStretchSame, ImVec2(w, 0))) {
        ImGui::TableNextRow();
        // ── left: Start + the running clients ("Recent") ──
        ImGui::TableNextColumn();
        heading("Start");
        for (size_t i = 0; i < cfg_.accounts.size(); ++i) {
            const Account& a = cfg_.accounts[i];
            ImGui::PushID(int(i));
            if (ui::link(fmt("Open %s", a.label.c_str()).c_str(), ui::Glyph::Play, true)) start_launch(a);
            ImGui::SameLine(0, 10);
            ImGui::TextColored(ui::kMuted, "login %s", a.login.c_str());
            ImGui::PopID();
        }
        if (ui::link("Open a capture...", ui::Glyph::File, true)) ImGui::OpenPopup("opencap");
        if (ImGui::BeginPopup("opencap")) {
            ImGui::TextDisabled("An exported capture (Debug > Packets > Export), replayed offline through the world model.");
            ImGui::SetNextItemWidth(520);
            ImGui::InputTextWithHint("##cappath", "path to a .txt capture", &capturePath_);
            ImGui::SameLine();
            if (ImGui::Button("Open")) {
                open_capture(capturePath_);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (ui::link("Settings...", ui::Glyph::Gear, true)) tab_ = Tab::Config;

        heading("Running clients");
        if (procs_.empty()) ImGui::TextColored(ui::kMuted, "no Trickster process running");
        for (const ProcEntry& p : procs_) {
            Session* linked = nullptr;
            for (auto& s : sessions_)
                if (s->pid == p.pid && !s->finished()) linked = s.get();
            ImGui::PushID((int)p.pid);
            if (ui::link(fmt("%s  #%u", p.exe.c_str(), p.pid).c_str(), ui::Glyph::Plug, true)) {
                if (linked) { curUid_ = linked->uid; tab_ = Tab::Bot; }
                else start_attach(p.pid);
            }
            ImGui::SameLine(0, 10);
            ImGui::TextColored(ui::kMuted, "%s", linked ? fmt("open: %s", linked->label.c_str()).c_str()
                                                 : p.hookLoaded ? "hook loaded - attach" : "attach (injects the hook)");
            ImGui::PopID();
        }
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::TextColored(ui::kMuted, "Attach only to a client of a server you run or are allowed to test.");

        // ── right: the open sessions, the setup ──
        ImGui::TableNextColumn();
        heading("Sessions");
        if (sessions_.empty()) ImGui::TextColored(ui::kMuted, "none yet");
        for (auto& sp : sessions_) {
            Session& s = *sp;
            const std::string name = s.world->self.name.empty() ? s.label : s.world->self.name;
            ImGui::PushID((int)s.uid);
            if (ui::link(name.c_str(), ui::Glyph::Bot, true)) { curUid_ = s.uid; selected_ = UINT64_MAX; tab_ = Tab::Bot; }
            ImGui::SameLine(0, 10);
            ImGui::TextColored(ui::kMuted, "%s  -  %s%s", s.label.c_str(), s.state_name(), s.bot.running ? "  -  bot running" : "");
            ImGui::PopID();
        }
        heading("Setup");
        auto row = [&](const char* k, const std::string& v, bool ok) {
            ImGui::TextColored(ok ? ui::kMuted : ui::kWarn, "%-8s", k);
            ImGui::SameLine(90);
            ImGui::TextColored(ok ? ui::kText : ui::kWarn, "%s", v.empty() ? "(not set)" : v.c_str());
        };
        row("client", cfg_.tricksterPath, !cfg_.tricksterPath.empty() && file_exists(cfg_.tricksterPath));
        row("server", fmt("%s:%d", cfg_.loginIp.c_str(), cfg_.loginPort), true);
        row("xml", gameData_.source(), gameData_.items() > 0);
        row("data", gameData_.map_dir(), gameData_.maps() > 0);
        row("icons", gfxStatus.empty() ? fmt("%zu", gfx::count()) : gfxStatus, gfxStatus.empty());
        if (ui::link("Edit the settings")) tab_ = Tab::Config;
        ImGui::EndTable();
    }
}

// ── Settings: tas.ini ──────────────────────────────────────────────────
void NeoApp::draw_config()
{
    if (!cfgEditLoaded_) { cfgEdit_ = cfg_; cfgEditLoaded_ = true; }
    Config& c = cfgEdit_;
    const float w = std::min(ImGui::GetContentRegionAvail().x, 760.0f);
    ImGui::PushFont(ui::fonts.big);
    ImGui::TextUnformatted("Settings");
    ImGui::PopFont();
    ImGui::TextColored(ui::kMuted, "Trickster Automation Studio  -  %s", cfg_.iniPath.c_str());
    ImGui::Dummy(ImVec2(0, 6));

    // VS Code's settings rows: "Group: Name", a muted line, the control
    auto setting = [&](const char* group, const char* name, const char* desc) {
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::TextColored(ui::kMuted, "%s:", group);
        ImGui::SameLine(0, 4);
        ImGui::PushFont(ui::fonts.bold);
        ImGui::TextUnformatted(name);
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kMuted);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        ImGui::TextWrapped("%s", desc);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(w);
    };
    setting("Client", "Trickster Path", "The client to launch (Trickster.bin or .exe; the other extension is tried too).");
    ImGui::InputText("##tp", &c.tricksterPath);
    setting("Client", "Original IP", "The server IP baked into your client build; every copy of it in the client's memory is "
                                     "replaced by the Login IP at launch (empty = the client is not patched).");
    ImGui::InputText("##oip", &c.originalIp);
    setting("Client", "Unpack Wait (ms)", "Wait after CreateProcess before patching the IP and injecting the hook DLL.");
    ImGui::InputInt("##uw", &c.unpackWaitMs, 100);
    setting("Client", "Pre-launch Command", "Run hidden before every launch (empty = nothing).");
    ImGui::InputText("##pre", &c.preLaunch);
    setting("Client", "Patch Login Globals", "Also write the login IP / port into the older build's globals (keep it off for Client_live).");
    ImGui::Checkbox("##plg", &c.patchLoginGlobals);
    setting("Server", "Login IP / Port", "Where the client logs in (the AuthServer).");
    ImGui::SetNextItemWidth(w * 0.6f);
    ImGui::InputText("##lip", &c.loginIp);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(w * 0.4f - 8);
    ImGui::InputInt("##lport", &c.loginPort, 0);
    setting("Server", "Group / Token", "The splash-bypass command line's group id and token.");
    ImGui::SetNextItemWidth(w * 0.3f);
    ImGui::InputText("##grp", &c.groupId);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(w * 0.7f - 8);
    ImGui::InputText("##tok", &c.token);
    setting("Data", "Server XML (libconfig)", "Your server's libconfig XML: items, monsters, skills, maps, NPCs, quests, shops, "
                                             "drops... every table is read from it (empty = the first conf\\*.xml next to the exe).");
    ImGui::InputText("##lib", &c.libConfig);
    setting("Data", "Server Data Folder", "Your server's / client's data folder: the maps (map\\*.md3) and the stat table "
                                         "(gulim13.fnt2) - the assets (empty = conf\\ next to the exe).");
    ImGui::InputText("##dd", &c.dataDir);
    setting("Data", "Map Dir (optional)", "Only when the .md3 maps are not in <Server Data Folder>\\map.");
    ImGui::InputText("##map", &c.mapDir);
    setting("Data", "YML Db Dir (optional)", "A folder of ProjectPAULA-style db\\*.yml files, read only when there is no XML.");
    ImGui::InputText("##gdb", &c.gameDbDir);
    setting("Bot", "Notify", "When a bot ends (the item hunt's list done, the quest's runs, a trip arrived) or stops on a "
                             "problem (died, no way, the session closed): a Windows notification, the taskbar button "
                             "flashing and a notice in the window. Not when you stop it.");
    ImGui::Checkbox("##ntf", &c.notifyStop);

    setting("Accounts", "Accounts", "Login (what the AuthServer resolves) and a label; each one is an \"Open\" on the Welcome page.");
    int del = -1;
    if (ImGui::BeginTable("accs", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg, ImVec2(w, 0))) {
        ImGui::TableSetupColumn("Login", 0, 160);
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", 0, 70);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < c.accounts.size(); ++i) {
            ImGui::PushID(int(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); ImGui::InputText("##l", &c.accounts[i].login);
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); ImGui::InputText("##n", &c.accounts[i].label);
            ImGui::TableNextColumn(); if (ImGui::Button("Remove")) del = int(i);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (del >= 0) c.accounts.erase(c.accounts.begin() + del);
    if (ImGui::Button("Add an account")) c.accounts.push_back({ fmt("%zu", c.accounts.size() + 1), "" });

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 4));
    if (ui::button_primary("Save", ImVec2(110, 0))) {
        std::string err;
        const bool dataChanged = c.gameDbDir != cfg_.gameDbDir || c.mapDir != cfg_.mapDir || c.libConfig != cfg_.libConfig ||
                                 c.dataDir != cfg_.dataDir;
        if (save_config(c, err)) {
            load_config();
            if (dataChanged) load_db();
            cfgEdit_ = cfg_;
            cfgStatus_ = "saved to " + cfg_.iniPath + (dataChanged ? " - game data reloaded" : "");
        } else {
            cfgStatus_ = err;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) { cfgEdit_ = cfg_; cfgStatus_ = "reverted"; }
    ImGui::SameLine();
    if (ImGui::Button("Reload game data")) { load_db(); cfgStatus_ = gameDataStatus_; }
    if (!cfgStatus_.empty()) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ui::kMuted, "%s", cfgStatus_.c_str());
    }
    ImGui::TextColored(ui::kMuted, "A launch uses the saved settings; open clients keep theirs.");
}

// ── Debug > Session: the links and the hook ─────────────────────────────────
void NeoApp::draw_session()
{
    if (sessions_.empty()) { ImGui::TextDisabled("No session. Open one from the Welcome page (+)."); return; }
    uint32_t removeUid = 0;
    if (ImGui::BeginTable("sessions", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Client");
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("State");
        ImGui::TableSetupColumn("Hooks");
        ImGui::TableSetupColumn("Packets", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Base", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 170);
        ImGui::TableHeadersRow();
        for (auto& sp : sessions_) {
            Session& s = *sp;
            ImGui::PushID((int)s.uid);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(s.label.c_str(), s.uid == curUid_)) {
                curUid_ = s.uid;
                selected_ = UINT64_MAX;
            }
            ImGui::TableNextColumn(); ImGui::Text("%u", s.pid);
            ImGui::TableNextColumn(); ImGui::TextColored(state_color(s.state), "%s", s.status.c_str());
            ImGui::TableNextColumn();
            if (s.haveHello)
                ImGui::TextColored(s.hookState == HOOK_ACTIVE ? kColOk : s.hookState == HOOK_FAILED ? kColErr : kColWarn,
                                   "%s", hook_state_name(s.hookState));
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)s.total_packets());
            ImGui::TableNextColumn();
            if (s.haveHello) ImGui::Text("0x%08X", s.hello.moduleBase);
            ImGui::TableNextColumn();
            if (!s.finished()) {
                if (ImGui::SmallButton("Unlink")) s.close();
                ImGui::SetItemTooltip("Drop the link. The DLL stays in the client and keeps capturing; attach again to resume.");
                ImGui::SameLine();
                if (ImGui::SmallButton("Kill client")) s.kill_client();
            } else if (ImGui::SmallButton("Close")) {
                removeUid = s.uid;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (removeUid) {
        sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                       [&](const std::unique_ptr<Session>& s) { return s->uid == removeUid; }),
                        sessions_.end());
        if (curUid_ == removeUid) curUid_ = sessions_.empty() ? 0 : sessions_.back()->uid;
    }
    if (Session* s = current(); s && s->haveHello) {
        ImGui::SeparatorText("Selected client");
        ImGui::Text("exe   %s", s->hello.exePath);
        ImGui::Text("hooks %s", hook_state_name(s->hookState));
        ImGui::Text("history replayed: %s   lost in DLL: %llu", s->replayDone ? "yes" : "in progress",
                    (unsigned long long)s->lostInDll);
        ImGui::Text("C2S connections seen: %zu", s->c2sConns.size());
    }
}

// ── Packets ─────────────────────────────────────────────────────────────────
void NeoApp::compile_filter()
{
    filter_.incOps.clear();
    filter_.excOps.clear();
    filter_.incText.clear();
    filter_.excText.clear();
    std::string tok;
    auto flush = [&] {
        if (tok.empty()) return;
        bool neg = tok[0] == '!';
        std::string t = neg ? tok.substr(1) : tok;
        uint32_t v;
        if (!t.empty()) {
            if (parse_uint(t, v)) (neg ? filter_.excOps : filter_.incOps).insert(v & 0xFFFF);
            else (neg ? filter_.excText : filter_.incText).push_back(lower(t));
        }
        tok.clear();
    };
    for (char c : filterExpr_) {
        if (c == ' ' || c == ',' || c == ';') flush();
        else tok += c;
    }
    flush();
    filter_.gen++;
}

bool NeoApp::filter_match(const CapPacket& p) const
{
    if (filter_.dir == 1 && p.hdr.dir != DIR_C2S) return false;
    if (filter_.dir == 2 && p.hdr.dir != DIR_S2C) return false;
    if (filter_.hideReplay && (p.hdr.flags & PF_REPLAY)) return false;
    if (filter_.excOps.count(p.hdr.opcode)) return false;
    const bool needName = !filter_.excText.empty() || !filter_.incText.empty();
    const std::string name = needName ? lower(packet_name(p.hdr.dir, p.hdr.opcode)) : std::string();
    for (const std::string& t : filter_.excText)
        if (name.find(t) != std::string::npos) return false;
    if (filter_.incOps.empty() && filter_.incText.empty()) return true;
    if (filter_.incOps.count(p.hdr.opcode)) return true;
    for (const std::string& t : filter_.incText)
        if (name.find(t) != std::string::npos) return true;
    return false;
}

bool NeoApp::update_view(Session& s)
{
    View& v = view_;
    if (v.sessionUid != s.uid || v.filterGen != filter_.gen) {
        v.rows.clear();
        v.sessionUid = s.uid;
        v.filterGen = filter_.gen;
        v.scannedUpTo = s.firstIndex;
    }
    if (!v.rows.empty() && v.rows.front() < s.firstIndex)
        v.rows.erase(v.rows.begin(), std::lower_bound(v.rows.begin(), v.rows.end(), s.firstIndex));
    if (v.scannedUpTo < s.firstIndex) v.scannedUpTo = s.firstIndex;
    if (paused_) return false;
    const size_t before = v.rows.size();
    const uint64_t total = s.total_packets();
    for (uint64_t i = v.scannedUpTo; i < total; ++i)
        if (filter_match(*s.packet_at(i))) v.rows.push_back(i);
    v.scannedUpTo = total;
    return v.rows.size() != before;
}

void NeoApp::draw_packets()
{
    Session* s = current();
    if (!s) {
        ImGui::TextDisabled("No client. Launch or attach one in Clients.");
        return;
    }

    // toolbar
    ImGui::SetNextItemWidth(110);
    if (ImGui::Combo("##dir", &filter_.dir, "All\0C>S only\0S>C only\0")) filter_.gen++;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(320);
    if (ImGui::InputTextWithHint("##filter", "opcodes / names, !x excludes  (e.g. 0x18 move !0x76C)", &filterExpr_))
        compile_filter();
    help_marker("Space-separated tokens. A number (24 or 0x18) keeps only those opcodes; text keeps packets "
                "whose name contains it; a leading ! excludes instead.");
    ImGui::SameLine();
    if (ImGui::Checkbox("Hide replay", &filter_.hideReplay)) filter_.gen++;
    ImGui::SetItemTooltip("Hide packets captured before this link (the history the DLL replays).");
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &follow_);
    ImGui::SameLine();
    ImGui::Checkbox("Pause view", &paused_);
    ImGui::SameLine();
    if (ImGui::Button("Export")) export_view(*s);
    ImGui::SetItemTooltip("Write the filtered rows (full hex) to captures\\ next to TricksterAutomationStudio.exe.");

    const bool grew = update_view(*s);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu / %llu", view_.rows.size(), (unsigned long long)s->total_packets());
    if (!exportStatus_.empty()) ImGui::TextDisabled("%s", exportStatus_.c_str());

    // list (resizable) + detail
    const float listH = ImGui::GetContentRegionAvail().y * 0.55f;
    ImGui::BeginChild("plist", ImVec2(0, listH), ImGuiChildFlags_ResizeY);
    const ImGuiTableFlags tf = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("packets", 7, tf)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Seq", 0, 60);
        ImGui::TableSetupColumn("Time", 0, 95);
        ImGui::TableSetupColumn("Dir", 0, 40);
        ImGui::TableSetupColumn("Opcode", 0, 110);
        ImGui::TableSetupColumn("Name", 0, 220);
        ImGui::TableSetupColumn("Len", 0, 50);
        ImGui::TableSetupColumn("Data", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        ImGuiListClipper clip;
        clip.Begin((int)view_.rows.size());
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const uint64_t idx = view_.rows[i];
                const CapPacket* p = s->packet_at(idx);
                if (!p) continue;
                const EvPacket& h = p->hdr;
                const bool replay = (h.flags & PF_REPLAY) != 0;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (replay) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                if (ImGui::Selectable(fmt("%llu##r%llu", (unsigned long long)h.seq, (unsigned long long)idx).c_str(),
                                      selected_ == idx, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                    selected_ = idx;
                    follow_ = false;
                }
                ImGui::TableNextColumn(); ImGui::TextUnformatted(filetime_local_hms(h.fileTime).c_str());
                ImGui::TableNextColumn();
                if (h.flags & PF_INJECTED) ImGui::TextColored(kColInjected, "C>S*");
                else ImGui::TextColored(h.dir == DIR_C2S ? kColC2S : kColS2C, "%s", dir_name(h.dir));
                ImGui::TableNextColumn(); ImGui::Text("0x%04X  %u", h.opcode, h.opcode);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(packet_name(h.dir, h.opcode).c_str());
                ImGui::TableNextColumn();
                if (h.flags & PF_TRUNCATED) ImGui::TextColored(kColWarn, "%u+", h.size);
                else ImGui::Text("%u", h.size);
                ImGui::TableNextColumn();
                if (monoFont) ImGui::PushFont(monoFont);
                ImGui::TextUnformatted(hex_preview(p->data.data(), p->data.size(), 40).c_str());
                if (monoFont) ImGui::PopFont();
                if (replay) ImGui::PopStyleColor();
            }
        }
        if (follow_ && grew) ImGui::SetScrollHereY(1.0f);
        ImGui::EndTable();
    }
    ImGui::EndChild();

    const CapPacket* sel = s->packet_at(selected_);
    ImGui::BeginChild("pdetail", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (sel) draw_packet_detail(*s, *sel);
    else ImGui::TextDisabled("Select a packet.");
    ImGui::EndChild();
}

void NeoApp::draw_packet_detail(Session& s, const CapPacket& p)
{
    const EvPacket& h = p.hdr;
    const PacketDef* def = db_.find(h.dir, h.opcode);

    ImGui::TextColored(h.dir == DIR_C2S ? kColC2S : kColS2C, "%s", h.dir == DIR_C2S ? "Client -> Server" : "Server -> Client");
    ImGui::SameLine();
    ImGui::Text("0x%04X (%u)  %s", h.opcode, h.opcode, def && !def->name.empty() ? def->name.c_str() : "(unknown)");
    ImGui::SameLine();
    ImGui::TextDisabled("seq %llu  %s  %u bytes  param %u  ctx 0x%08X%s%s%s", (unsigned long long)h.seq,
                        filetime_local_hms(h.fileTime).c_str(), h.size, h.param, h.ctx,
                        h.flags & PF_INJECTED ? "  injected" : "", h.flags & PF_TRUNCATED ? "  TRUNCATED" : "",
                        h.flags & PF_REPLAY ? "  replay" : "");
    if (def) {
        if (!def->handler.empty()) ImGui::TextDisabled("handler %s", def->handler.c_str());
        if (!def->note.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("note: %s", def->note.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::TextDisabled("source: %s", def->source.c_str());
        if (!def->fieldError.empty()) ImGui::TextColored(kColErr, "fields: %s", def->fieldError.c_str());
    }

    if (ImGui::SmallButton("Copy hex")) ImGui::SetClipboardText(hex_preview(p.data.data(), p.data.size(), p.data.size()).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Edit & send")) {
        load_into_editor(p);
        tab_ = Tab::Send;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Only this opcode")) {
        filterExpr_ = fmt("0x%X", h.opcode);
        compile_filter();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Hide this opcode")) {
        filterExpr_ += fmt(" !0x%X", h.opcode);
        compile_filter();
    }
    (void)s;

    if (ImGui::BeginTable("detail", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("decoded", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("hex", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::BeginChild("decoded", ImVec2(0, 0));
        if (def && !def->fields.empty()) {
            std::vector<DecodedRow> rows;
            db_.decode(*def, p.data.data(), p.data.size(), rows);
            if (ImGui::BeginTable("fields", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Off", 0, 45);
                ImGui::TableSetupColumn("Field", 0, 150);
                ImGui::TableSetupColumn("Type", 0, 80);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                ImGuiListClipper clip;
                clip.Begin((int)rows.size());
                while (clip.Step()) {
                    for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                        const DecodedRow& r = rows[i];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::TextDisabled("%u", r.off);
                        ImGui::TableNextColumn();
                        // Indent(0) would indent by the default spacing
                        if (r.depth) ImGui::Indent(r.depth * 12.0f);
                        ImGui::TextUnformatted(r.label.c_str());
                        if (r.depth) ImGui::Unindent(r.depth * 12.0f);
                        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", r.type.c_str());
                        ImGui::TableNextColumn();
                        if (r.error) ImGui::TextColored(kColErr, "%s", r.value.c_str());
                        else if (r.trailing) ImGui::TextColored(kColWarn, "%s", r.value.c_str());
                        else ImGui::TextUnformatted(r.value.c_str());
                    }
                }
                ImGui::EndTable();
            }
        } else {
            ImGui::TextDisabled(def ? "No field layout for this packet yet." : "Unknown opcode.");
            if (def && !def->fieldsRaw.empty()) ImGui::TextDisabled("fields: %s", def->fieldsRaw[0].c_str());
        }
        ImGui::EndChild();

        ImGui::TableNextColumn();
        ImGui::BeginChild("hex", ImVec2(0, 0), 0, ImGuiWindowFlags_HorizontalScrollbar);
        draw_hex(p.data.data(), p.data.size());
        ImGui::EndChild();
        ImGui::EndTable();
    }
}

void NeoApp::draw_hex(const uint8_t* p, size_t n)
{
    if (monoFont) ImGui::PushFont(monoFont);
    const int lines = (int)((n + 15) / 16);
    ImGuiListClipper clip;
    clip.Begin(lines);
    char line[100];
    while (clip.Step()) {
        for (int l = clip.DisplayStart; l < clip.DisplayEnd; ++l) {
            const size_t off = (size_t)l * 16;
            int k = snprintf(line, sizeof(line), "%04zX  ", off);
            for (size_t i = 0; i < 16; ++i) {
                if (off + i < n) k += snprintf(line + k, sizeof(line) - k, "%02X ", p[off + i]);
                else k += snprintf(line + k, sizeof(line) - k, "   ");
                if (i == 7) line[k++] = ' ';
            }
            line[k++] = ' ';
            for (size_t i = 0; i < 16 && off + i < n; ++i) {
                uint8_t c = p[off + i];
                line[k++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
            }
            line[k] = '\0';
            ImGui::TextUnformatted(line);
        }
    }
    if (!n) ImGui::TextDisabled("(empty payload)");
    if (monoFont) ImGui::PopFont();
}

void NeoApp::export_view(Session& s)
{
    const std::string dir = exe_dir() + "captures\\";
    CreateDirectoryA(dir.c_str(), nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    const std::string path = dir + fmt("pid%u_%04u%02u%02u_%02u%02u%02u.txt", s.pid, st.wYear, st.wMonth, st.wDay,
                                       st.wHour, st.wMinute, st.wSecond);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        exportStatus_ = "cannot write " + path;
        return;
    }
    out << "# Trickster Automation Studio capture - " << s.label << " - filter: " << filterExpr_ << "\r\n";
    out << "# seq\ttime\tdir\topcode\tsize\tflags\tname\tpayload hex   (Clients > Open capture reads it back)\r\n";
    size_t n = 0;
    for (uint64_t idx : view_.rows) {
        const CapPacket* p = s.packet_at(idx);
        if (!p) continue;
        const EvPacket& h = p->hdr;
        std::string flags;
        if (h.flags & PF_INJECTED) flags += 'i';
        if (h.flags & PF_TRUNCATED) flags += 't';
        if (h.flags & PF_REPLAY) flags += 'r';
        out << fmt("%llu\t%s\t%s\t0x%04X\t%u\t%s\t%s\t", (unsigned long long)h.seq, filetime_local_hms(h.fileTime).c_str(),
                   dir_name(h.dir), h.opcode, h.size, flags.empty() ? "-" : flags.c_str(),
                   packet_name(h.dir, h.opcode).c_str())
            << hex_preview(p->data.data(), p->data.size(), p->data.size()) << "\r\n";
        ++n;
    }
    exportStatus_ = fmt("exported %zu packets to %s", n, path.c_str());
    app_log("%s", exportStatus_.c_str());
}

// ── Opcodes ─────────────────────────────────────────────────────────────────
void NeoApp::draw_opcodes()
{
    Session* s = current();
    if (!s) {
        ImGui::TextDisabled("No client.");
        return;
    }
    struct Row { uint8_t dir; uint16_t op; const OpStat* st; std::string name; };
    std::vector<Row> rows;
    rows.reserve(s->opStats.size());
    for (auto& kv : s->opStats)
        rows.push_back({ (uint8_t)(kv.first >> 16), (uint16_t)kv.first, &kv.second,
                         packet_name((uint8_t)(kv.first >> 16), (uint16_t)kv.first) });

    ImGui::TextDisabled("%zu distinct opcodes. Double-click a row to filter the packet list to it.", rows.size());
    const ImGuiTableFlags tf = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("opcodes", 6, tf)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Dir", 0, 45);
    ImGui::TableSetupColumn("Opcode", ImGuiTableColumnFlags_DefaultSort, 110);
    ImGui::TableSetupColumn("Name", 0, 260);
    ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_PreferSortDescending, 80);
    ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_PreferSortDescending, 90);
    ImGui::TableSetupColumn("Last seq", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableHeadersRow();

    if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs(); ss && ss->SpecsCount > 0) {
        const ImGuiTableColumnSortSpecs& sp = ss->Specs[0];
        const bool asc = sp.SortDirection == ImGuiSortDirection_Ascending;
        std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
            int c = 0;
            switch (sp.ColumnIndex) {
            case 0: c = (int)a.dir - (int)b.dir; break;
            case 1: c = a.op != b.op ? (a.op < b.op ? -1 : 1) : (int)a.dir - (int)b.dir; break;
            case 2: c = a.name.compare(b.name); break;
            case 3: c = a.st->count == b.st->count ? 0 : (a.st->count < b.st->count ? -1 : 1); break;
            case 4: c = a.st->bytes == b.st->bytes ? 0 : (a.st->bytes < b.st->bytes ? -1 : 1); break;
            default: c = a.st->lastSeq == b.st->lastSeq ? 0 : (a.st->lastSeq < b.st->lastSeq ? -1 : 1); break;
            }
            return asc ? c < 0 : c > 0;
        });
    }

    for (const Row& r : rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID((int)((uint32_t)r.dir << 16 | r.op));
        if (ImGui::Selectable("##row", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
            ImGui::IsMouseDoubleClicked(0)) {
            filterExpr_ = fmt("0x%X", r.op);
            filter_.dir = r.dir == DIR_C2S ? 1 : 2;
            compile_filter();
            tab_ = Tab::Packets;
        }
        ImGui::PopID();
        ImGui::SameLine();
        ImGui::TextColored(r.dir == DIR_C2S ? kColC2S : kColS2C, "%s", dir_name(r.dir));
        ImGui::TableNextColumn(); ImGui::Text("0x%04X  %u", r.op, r.op);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(r.name.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)r.st->count);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)r.st->bytes);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)r.st->lastSeq);
    }
    ImGui::EndTable();
}

// ── Send ────────────────────────────────────────────────────────────────────
void NeoApp::load_into_editor(const CapPacket& p)
{
    sendOp_ = fmt("0x%04X", p.hdr.opcode);
    sendParam_ = p.hdr.param;
    sendHex_ = hex_preview(p.data.data(), p.data.size(), p.data.size());
    editorFromS2C_ = p.hdr.dir == DIR_S2C;
    sendCtx_ = p.hdr.dir == DIR_C2S ? p.hdr.ctx : 0;
    sendFieldMode_ = false;                         // a captured body is raw bytes
    sendStatus_.clear();
}

void NeoApp::set_editor(uint16_t op, const std::vector<uint8_t>& body, uint32_t ctx)
{
    sendOp_ = fmt("0x%04X", op);
    sendParam_ = 0;
    sendHex_ = hex_preview(body.data(), body.size(), body.size());
    sendCtx_ = ctx;
    editorFromS2C_ = false;
    sendFieldMode_ = false;                         // quick actions fill the raw bytes
    sendStatus_.clear();
}

void NeoApp::do_action(Session& s, const Action& a)
{
    if (!a.ok()) {
        actionStatus_ = fmt("0x%04X: %s", a.op, a.error.c_str());
        actionStatusOk_ = false;
        return;
    }
    set_editor(a.op, a.body, s.gameCtx);             // the game link (0 = auto until one is seen)
    actionStatus_ = a.label;
    actionStatusOk_ = true;
    if (!quickSend_) return;
    std::string err;
    if (s.state == Session::State::Live && s.send(s.gameCtx, a.op, 0, a.body, err)) {
        sendStatus_ = fmt("queued #%u (%zu bytes)", s.sends.back().reqId, a.body.size());
        sendStatusOk_ = true;
    } else {
        sendStatus_ = s.state == Session::State::Live ? err : std::string("session not live");
        sendStatusOk_ = false;
    }
}

// Buttons that build a C2S action from the live world state (actions.cpp: the layouts and the server
// checks of each one). They fill the editor; with "Send at once" they also send it.
void NeoApp::draw_quick_actions(Session& s)
{
    const WorldModel& w = *s.world;
    const uint64_t now = now_filetime();

    ImGui::Checkbox("Send at once", &quickSend_);
    ImGui::SameLine();
    help_marker("Off: a button only fills the editor below - review it and click Send.\n"
                "On: the button sends it too (radar right-clicks as well).");
    ImGui::SameLine();
    ImGui::Checkbox("Run", &quickRun_);
    ImGui::SameLine();
    help_marker("The move type the requests carry (2 run / 1 walk; walk = half the run speed). Attack, pick-up "
                "and move all take it: the server walks us with it.");
    if (!actionStatus_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(actionStatusOk_ ? kColOk : kColErr, "%s", actionStatus_.c_str());
    }

    // combat / pick-up / sit
    const Entity* nearMob = nearest_monster(w, now);
    const GroundItem* nearItem = nearest_item(w, now);
    ImGui::BeginDisabled(nearMob == nullptr);
    if (ImGui::Button(nearMob ? fmt("Attack %s", w.entity_label(nearMob->eid).c_str()).c_str() : "Attack (no monster)"))
        do_action(s, act::attack(w, nearMob->eid, quickRun_));
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(nearItem == nullptr);
    if (ImGui::Button(nearItem ? fmt("Pick up %s", gameData_.item_name(nearItem->itemId).c_str()).c_str()
                               : "Pick up (no item)"))
        do_action(s, act::pick_up(w, nearItem->oid, quickRun_));
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Sit"))   do_action(s, act::sit(2));
    ImGui::SameLine();
    if (ImGui::Button("Stand")) do_action(s, act::sit(0));
    ImGui::SameLine();
    if (ImGui::Button("Rest"))  do_action(s, act::sit(13));
    ImGui::SameLine();
    help_marker("Attack = C2S 0x1D [target][type] (the server chases and swings); Pick up = 0x21 [oid][type]; "
                "Sit / Stand / Rest = 0x40 [2 / 0 / 13].");

    // skill
    {
        const SkillEntry* cur = nullptr;
        for (const SkillEntry& sk : w.skills)
            if (sk.id == skillSel_) cur = &sk;
        if (!cur) {                                  // first usable one
            for (const SkillEntry& sk : w.skills)
                if (usable_skill(w, sk)) { cur = &sk; skillSel_ = sk.id; break; }
        }
        auto skill_label = [&](const SkillEntry& sk) {
            const SkillInfo* si = gameData_.skill(sk.id);
            const char* how = !si ? "" : si->esaui == 0 ? ", no target" : esaui_point(si->esaui) ? ", point" : "";
            return fmt("%s Lv %u  (%u%s)", gameData_.skill_name(sk.id).c_str(), sk.level, sk.id, how);
        };
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Skill");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo("##skill", cur ? skill_label(*cur).c_str() : "(no skill list yet)")) {
            for (const SkillEntry& sk : w.skills) {
                std::string why;
                const bool ok = usable_skill(w, sk, &why);
                ImGui::BeginDisabled(!ok);
                if (ImGui::Selectable(skill_label(sk).c_str(), cur && cur->id == sk.id)) skillSel_ = sk.id;
                ImGui::EndDisabled();
                if (!ok) ImGui::SetItemTooltip("%s", why.c_str());
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        static const char* kModes[] = { "auto", "self", "nearest monster", "entity #", "point (move X/Y)" };
        ImGui::SetNextItemWidth(130);
        ImGui::Combo("##stgt", &skillTargetMode_, kModes, IM_ARRAYSIZE(kModes));
        if (skillTargetMode_ == SkillTarget::Entity) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::InputInt("##steid", &skillTargetEid_, 0);
            skillTargetEid_ = std::clamp(skillTargetEid_, 0, 0xFFFF);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(cur == nullptr);
        if (ImGui::Button("Use skill") && cur) {
            SkillTarget t;
            t.mode = SkillTarget::Mode(skillTargetMode_);
            t.eid = uint16_t(skillTargetEid_);
            t.x = moveX_; t.y = moveY_;
            do_action(s, act::use_skill(w, cur->id, t, now));
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        help_marker("A skill without a selection UI (ESAUI 0) = C2S 0xEE [skill][lv]. With one = the client's answer "
                    "to S2C 241 directly: 0xE0 [2][skill][lv][ESAUI][size][target eid | x,y] (ESAUI 1/7/8 take a "
                    "target, 3/6/10 a point). The server needs no 0xEE first, so the client's cursor never opens.\n"
                    "auto = a buff (Type 3) on yourself, anything else on the nearest live monster.\n"
                    "Passive / unknown skills are greyed out: the server drops the session for them.");
    }

    // item
    {
        const InvItem* cur = itemSel_ ? w.find_item(itemSel_) : nullptr;
        if (cur && !usable_item(w, *cur)) cur = nullptr;
        if (!cur) {                                  // first potion, else the first usable item
            const InvItem* any = nullptr;
            for (const InvItem& it : w.inventory) {
                if (!usable_item(w, it)) continue;
                const ItemInfo* ii = gameData_.item(it.itemId);
                if (!any) any = &it;
                if (ii && ii->type == 10) { cur = &it; break; }
            }
            if (!cur) cur = any;
            itemSel_ = cur ? cur->uid : 0;
        }
        auto item_label = [&](const InvItem& it) {
            const ItemInfo* ii = gameData_.item(it.itemId);
            std::string extra;
            if (ii && ii->hp) extra += fmt(" HP+%d", ii->hp);
            if (ii && ii->mp) extra += fmt(" MP+%d", ii->mp);
            return fmt("%s x%u%s", gameData_.item_name(it.itemId).c_str(), it.count, extra.c_str());
        };
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Item ");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo("##item", cur ? item_label(*cur).c_str() : "(nothing usable in the bag)")) {
            for (const InvItem& it : w.inventory) {
                if (!usable_item(w, it)) continue;
                ImGui::PushID((int)(it.uid & 0x7FFFFFFF));
                if (ImGui::Selectable(item_label(it).c_str(), cur && cur->uid == it.uid)) itemSel_ = it.uid;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(cur == nullptr);
        if (ImGui::Button("Use item") && cur) do_action(s, act::use_item(w, cur->uid));
        ImGui::EndDisabled();
        ImGui::SameLine();
        help_marker("C2S 0x57 [u64 uid]. Lists the bag items with ItemParam Options & 2 (usable), potions first. "
                    "A potion answers with the block {0x97, 0x5B, 0x24 / 0x25}; 0x5A = refused.");
    }

    // move
    {
        int px, py;
        w.self_pos_at(now, px, py);
        if (!moveSet_) { moveX_ = px; moveY_ = py; }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Move ");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        if (ImGui::InputInt("##mx", &moveX_, 10)) moveSet_ = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        if (ImGui::InputInt("##my", &moveY_, 10)) moveSet_ = true;
        moveX_ = std::clamp(moveX_, 0, 0xFFFF);
        moveY_ = std::clamp(moveY_, 0, 0xFFFF);
        ImGui::SameLine();
        if (ImGui::Button("Here")) { moveX_ = px; moveY_ = py; moveSet_ = false; }
        ImGui::SameLine();
        if (ImGui::Button("Move")) do_action(s, act::move(w, moveX_, moveY_, quickRun_, now));
        ImGui::SameLine();
        if (ImGui::Button("Portal walk")) do_action(s, act::portal_walk(moveX_, moveY_, quickRun_));
        ImGui::SameLine();
        ImGui::TextDisabled("now (%d, %d)", px, py);
        ImGui::SameLine();
        help_marker("Move = C2S 0x18 [from][to][type], from = where the model has us now (the move segment "
                    "interpolated at the S2C 0x1D8 speed). The server echoes S2C 0x18 to us and the client walks "
                    "on it, as on the server's own chase.\nPortal walk = 0x1D4 [to][type]: the same walk from "
                    "where the SERVER has us, with the portal armed (the client sends it for a click in a portal).\n"
                    "Right-click on the World radar sets the point and does a Move.");
    }
}

void NeoApp::draw_send()
{
    Session* s = current();
    if (!s) {
        ImGui::TextDisabled("No client.");
        return;
    }
    ImGui::TextDisabled("Sends a C2S packet through the client's own Net_SendPacket_Raw: the client frames and "
                        "encrypts it like one of its own.");
    if (editorFromS2C_)
        ImGui::TextColored(kColWarn, "Loaded from a S>C packet: Send still goes client -> server.");

    ImGui::SeparatorText("Quick actions");
    draw_quick_actions(*s);

    // connection
    ImGui::SeparatorText("Packet");
    std::string connLabel = sendCtx_ ? fmt("0x%08X", sendCtx_) : std::string("auto (the client's last C>S connection)");
    ImGui::SetNextItemWidth(420);
    if (ImGui::BeginCombo("Connection", connLabel.c_str())) {
        if (ImGui::Selectable("auto (the client's last C>S connection)", sendCtx_ == 0)) sendCtx_ = 0;
        for (auto& kv : s->c2sConns)
            if (ImGui::Selectable(fmt("0x%08X  (%llu C>S packets)", kv.first, (unsigned long long)kv.second).c_str(),
                                  sendCtx_ == kv.first))
                sendCtx_ = kv.first;
        ImGui::EndCombo();
    }
    help_marker("The client keeps one connection per server (login, game, community). A packet loaded from the "
                "list keeps the connection it was captured on.");

    ImGui::SetNextItemWidth(120);
    ImGui::InputTextWithHint("Opcode", "0x18 or 24", &sendOp_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::InputInt("Param", &sendParam_);
    sendParam_ = std::clamp(sendParam_, 0, 0xFFFF);

    uint32_t op = 0;
    const bool opOk = parse_uint(sendOp_, op) && op <= 0xFFFF;
    const PacketDef* def = opOk ? db_.find(DIR_C2S, (uint16_t)op) : nullptr;
    if (opOk) {
        if (def) {
            ImGui::TextColored(kColC2S, "%s", def->name.empty() ? "(no name)" : def->name.c_str());
            if (!def->note.empty()) {
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled("%s", def->note.c_str());
                ImGui::PopTextWrapPos();
            }
        } else {
            ImGui::TextDisabled("opcode not in the packet db");
        }
    }

    // field editor when the opcode has a flat layout, else raw hex
    const bool canFields = def && PacketDb::editable_fields(*def);
    ImGui::BeginDisabled(!canFields);
    ImGui::Checkbox("Edit by fields", &sendFieldMode_);
    ImGui::EndDisabled();
    if (!canFields) {
        sendFieldMode_ = false;
        if (def && !def->fieldsRaw.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(layout not flat: %s)", def->fieldError.empty() ? "has loops/tuples"
                                                                                 : def->fieldError.c_str());
        }
    }

    std::vector<uint8_t> payload;
    std::string payErr;
    bool payOk = false;

    if (sendFieldMode_ && canFields) {
        if (sendFieldsFor_ != (int)op) {             // opcode changed: reset the inputs
            sendFieldsFor_ = (int)op;
            sendFieldVals_.assign(def->fields.size(), std::string());
        }
        sendFieldVals_.resize(def->fields.size());
        int bad = -1;
        PacketDb::encode(*def, sendFieldVals_, payload, payErr, bad);   // preview for the badField highlight
        if (ImGui::BeginTable("fieldedit", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                              ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Field", 0, 160);
            ImGui::TableSetupColumn("Type", 0, 70);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < def->fields.size(); ++i) {
                const FieldSpec& f = def->fields[i];
                ImGui::TableNextRow();
                ImGui::PushID((int)i);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(f.name.empty() ? fmt("f%zu", i).c_str() : f.name.c_str());
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", f.type.c_str());
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                const bool isText = f.kind == FieldSpec::Kind::Str || f.kind == FieldSpec::Kind::FixedStr;
                const bool isHex = f.kind == FieldSpec::Kind::Blob || f.kind == FieldSpec::Kind::Rest;
                ImGui::InputTextWithHint("##v", isText ? "text" : isHex ? "hex bytes" : "number (0x ok)",
                                         &sendFieldVals_[i]);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        // rebuild from the (possibly edited) values
        payOk = PacketDb::encode(*def, sendFieldVals_, payload, payErr, bad);
        if (payOk) sendHex_ = hex_preview(payload.data(), payload.size(), payload.size());  // keep hex view in sync
        ImGui::TextDisabled("= %s", payload.empty() ? "(empty)" : hex_preview(payload.data(), payload.size(), 48).c_str());
    } else {
        ImGui::TextDisabled("Payload (hex, without the 11-byte header)");
        if (monoFont) ImGui::PushFont(monoFont);
        ImGui::InputTextMultiline("##payload", &sendHex_, ImVec2(-1, ImGui::GetTextLineHeight() * 7));
        if (monoFont) ImGui::PopFont();
        payOk = parse_hex(sendHex_, payload, payErr);
        sendFieldsFor_ = -1;                          // force a field reset next time fields mode opens
    }
    if (payOk) ImGui::TextDisabled("%zu bytes", payload.size());
    else ImGui::TextColored(kColErr, "%s", payErr.c_str());

    ImGui::BeginDisabled(!opOk || !payOk || s->state != Session::State::Live);
    if (ImGui::Button("Send", ImVec2(120, 0))) {
        std::string err;
        if (s->send(sendCtx_, (uint16_t)op, (uint16_t)sendParam_, payload, err)) {
            sendStatus_ = fmt("queued #%u (%zu bytes)", s->sends.back().reqId, payload.size());
            sendStatusOk_ = true;
        } else {
            sendStatus_ = err;
            sendStatusOk_ = false;
        }
    }
    ImGui::EndDisabled();
    if (s->state != Session::State::Live) {
        ImGui::SameLine();
        ImGui::TextDisabled("(session not live)");
    }
    if (!sendStatus_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(sendStatusOk_ ? kColOk : kColErr, "%s", sendStatus_.c_str());
    }

    ImGui::SeparatorText("Sent");
    if (ImGui::BeginTable("sent", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                     ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", 0, 50);
        ImGui::TableSetupColumn("Time", 0, 95);
        ImGui::TableSetupColumn("Opcode", 0, 200);
        ImGui::TableSetupColumn("Len", 0, 50);
        ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (auto it = s->sends.rbegin(); it != s->sends.rend(); ++it) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%u", it->reqId);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(it->when.c_str());
            ImGui::TableNextColumn(); ImGui::Text("0x%04X %s", it->op, packet_name(DIR_C2S, it->op).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%u", it->len);
            ImGui::TableNextColumn();
            if (!it->done) ImGui::TextColored(kColWarn, "pending");
            else if (it->ok) ImGui::TextColored(kColOk, "sent");
            else ImGui::TextColored(kColErr, "%s", it->error.c_str());
        }
        ImGui::EndTable();
    }
}

// ── Log ─────────────────────────────────────────────────────────────────────
void NeoApp::draw_log()
{
    if (ImGui::Button("Clear")) app_log_clear();
    ImGui::SameLine();
    if (ImGui::Button("Copy all")) {
        std::string all;
        app_log_read([&](const std::deque<std::string>& l) {
            for (const std::string& x : l) all += x + "\n";
        });
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    const uint64_t ver = app_log_version();
    app_log_read([&](const std::deque<std::string>& lines) {
        ImGuiListClipper clip;
        clip.Begin((int)lines.size());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) ImGui::TextUnformatted(lines[i].c_str());
    });
    if (ver != logSeen_) {
        ImGui::SetScrollHereY(1.0f);
        logSeen_ = ver;
    }
    ImGui::EndChild();
}

} // namespace nj
