#include "app/libconfig.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <unordered_set>

namespace nj {

namespace {

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

std::string decode(const std::string& s)
{
    if (s.find('&') == std::string::npos) return s;
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o += s[i]; continue; }
        const size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 8) { o += s[i]; continue; }
        const std::string e = s.substr(i + 1, semi - i - 1);
        if (e == "amp") o += '&';
        else if (e == "lt") o += '<';
        else if (e == "gt") o += '>';
        else if (e == "quot") o += '"';
        else if (e == "apos") o += '\'';
        else if (!e.empty() && e[0] == '#') {
            const unsigned long c = e.size() > 1 && (e[1] == 'x' || e[1] == 'X') ? std::strtoul(e.c_str() + 2, nullptr, 16)
                                                                              : std::strtoul(e.c_str() + 1, nullptr, 10);
            if (c < 0x80) o += char(c);
            else if (c < 0x800) { o += char(0xC0 | (c >> 6)); o += char(0x80 | (c & 0x3F)); }
            else { o += char(0xE0 | (c >> 12)); o += char(0x80 | ((c >> 6) & 0x3F)); o += char(0x80 | (c & 0x3F)); }
        } else { o += s.substr(i, semi - i + 1); }
        i = semi;
    }
    return o;
}

// "4/16/128" -> "148"
std::string enum_or(const std::string& v)
{
    long long acc = 0;
    size_t p = 0;
    while (p <= v.size()) {
        size_t q = v.find('/', p);
        if (q == std::string::npos) q = v.size();
        acc |= std::strtoll(trim(v.substr(p, q - p)).c_str(), nullptr, 10);
        p = q + 1;
    }
    return std::to_string(acc);
}

bool numeric_zero(const std::string& v)
{
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    return end && *end == 0 && end != v.c_str() && d == 0.0;
}

std::string attr(const std::string& t, size_t from, size_t to, const char* name)
{
    const std::string key = std::string(name) + "=\"";
    const size_t a = t.find(key, from);
    if (a == std::string::npos || a >= to) return std::string();
    const size_t b = t.find('"', a + key.size());
    return b == std::string::npos ? std::string() : t.substr(a + key.size(), b - a - key.size());
}

} // namespace

bool LibConfig::load(const std::string& path, std::string& err)
{
    text_.clear();
    index_.clear();
    order_.clear();
    path_ = path;
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open " + path; return false; }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size <= 0) { err = path + " is empty"; return false; }
    text_.resize(size_t(size));
    in.read(&text_[0], size);
    static const char kOpen[] = "<TABLE name=\"";
    for (size_t p = text_.find(kOpen); p != std::string::npos; p = text_.find(kOpen, p)) {
        const size_t n0 = p + sizeof(kOpen) - 1, n1 = text_.find('"', n0);
        const size_t gt = n1 == std::string::npos ? std::string::npos : text_.find('>', n1);
        const size_t end = gt == std::string::npos ? std::string::npos : text_.find("</TABLE>", gt);
        if (end == std::string::npos) break;
        const std::string name = text_.substr(n0, n1 - n0);
        if (!index_.count(name)) order_.push_back(name);
        index_[name] = Span{ gt + 1, end };
        p = end;
    }
    if (index_.empty()) { err = path + ": no <TABLE> in it"; text_.clear(); return false; }
    return true;
}

std::vector<std::string> LibConfig::with_prefix(const std::string& prefix) const
{
    std::vector<std::string> out;
    for (const std::string& n : order_)
        if (n.compare(0, prefix.size(), prefix) == 0) out.push_back(n);
    return out;
}

bool LibConfig::rows(const std::string& table, std::vector<YRecord>& out, const char* tableField) const
{
    auto it = index_.find(table);
    if (it == index_.end()) return false;
    const std::string& t = text_;
    const size_t b = it->second.begin, e = it->second.end;
    size_t firstRow = t.find("<ROW>", b);
    if (firstRow == std::string::npos || firstRow > e) firstRow = e;
    // the fields' types
    std::unordered_map<std::string, char> kind;             // 'E' Enum, 'S' String, 'N' number / bool
    for (size_t f = t.find("<FIELDINFO", b); f != std::string::npos && f < firstRow; f = t.find("<FIELDINFO", f + 10)) {
        const size_t fe = t.find('>', f);
        const std::string dt = attr(t, f, fe, "DataType");
        kind[attr(t, f, fe, "Name")] = dt == "Enum" ? 'E' : dt == "String" ? 'S' : 'N';
    }
    int line = 0;
    for (size_t r = firstRow; r < e; ) {
        r = t.find("<ROW>", r);
        if (r == std::string::npos || r >= e) break;
        const size_t re = t.find("</ROW>", r);
        if (re == std::string::npos || re > e) break;
        YRecord rec;
        rec.line = ++line;
        if (tableField) rec.scalars[tableField] = table;
        size_t q = r + 5;
        while (q < re) {
            const size_t lt = t.find('<', q);
            if (lt == std::string::npos || lt >= re) break;
            const size_t gt = t.find('>', lt);
            if (gt == std::string::npos || gt > re) break;
            if (t[lt + 1] == '/') { q = gt + 1; continue; }
            size_t ne = lt + 1;
            while (ne < gt && t[ne] != ' ' && t[ne] != '/') ++ne;
            const std::string name = t.substr(lt + 1, ne - lt - 1);
            if (t[gt - 1] == '/') { q = gt + 1; continue; }     // <Field/>: empty
            std::string value;
            size_t close;
            if (t.compare(gt + 1, 9, "<![CDATA[") == 0) {
                const size_t ce = t.find("]]>", gt + 10);
                if (ce == std::string::npos || ce > re) break;
                value = t.substr(gt + 10, ce - gt - 10);
                close = t.find("</", ce + 3);
            } else {
                close = t.find("</", gt + 1);
                if (close == std::string::npos || close > re) break;
                value = decode(t.substr(gt + 1, close - gt - 1));
            }
            if (close == std::string::npos || close > re) break;
            const size_t ce = t.find('>', close);
            q = ce == std::string::npos ? re : ce + 1;
            value = trim(value);
            if (value.empty()) continue;
            auto k = kind.find(name);
            const char kd = k == kind.end() ? 'N' : k->second;
            if (kd == 'E' && value.find('/') != std::string::npos) value = enum_or(value);
            if (kd != 'S' && numeric_zero(value)) continue;
            rec.scalars[name] = std::move(value);
        }
        out.push_back(std::move(rec));
        r = re + 6;
    }
    return true;
}

// ── the ProjectPAULA db files from the libconfig ─────────────────────────────
bool libconfig_db(const LibConfig& lc, const std::string& db, std::vector<YRecord>& out, std::string& err)
{
    auto one = [&](const char* table) {
        if (lc.rows(table, out)) return true;
        err = std::string("no table ") + table + " in " + lc.path();
        return false;
    };
    // the values of a field over a table's rows (each once, in order)
    auto refs = [&](const std::vector<YRecord>& rs, const char* field, std::vector<std::string>& names, std::set<std::string>& seen) {
        for (const YRecord& r : rs) {
            const std::string v = r.str(field);
            if (!v.empty() && seen.insert(v).second) names.push_back(v);
        }
    };
    auto map_rows = [&]() {
        std::vector<YRecord> all, kept;
        lc.rows("MapInfoEx", all);
        for (YRecord& r : all) {
            const long long id = r.num("ID");
            if (id >= 1 && id <= 999) kept.push_back(std::move(r));      // (the retail InitializeMapInfo keeps 1..999)
        }
        return kept;
    };
    auto tables = [&](const std::vector<std::string>& names) {
        for (const std::string& n : names) lc.rows(n, out, "Table");
        return true;
    };
    auto char_names = [&]() {
        std::unordered_map<long long, std::string> m;
        std::vector<YRecord> rs;
        lc.rows("CharacterInfo", rs);
        for (const YRecord& r : rs) {
            std::string n = r.str("CommonName");
            if (n.empty()) n = r.str("CommonName_Eng");
            m[r.num("ID")] = n;
        }
        return m;
    };
    auto npc_tables = [&](std::vector<std::string>& names) {
        std::set<std::string> seen;
        refs(map_rows(), "NpcTable", names, seen);
    };

    if (db == "item_db") {                                  // the four item tables (the retail GameInfoMgr merges them)
        std::vector<YRecord> rs;
        std::unordered_set<long long> ids;
        for (const char* t : { "ItemParam2", "ItemParamEx2", "ItemParamCM2", "ItemParamEx" }) {
            rs.clear();
            lc.rows(t, rs);
            for (YRecord& r : rs)
                if (ids.insert(r.num("ID")).second) out.push_back(std::move(r));
        }
        if (out.empty()) { err = "no ItemParam2 in " + lc.path(); return false; }
        return true;
    }
    if (db == "mob_db") {                                   // MonsterParamEx2, with the yml's names
        if (!one("MonsterParamEx2")) return false;
        static const char* const kAlias[][2] = { { "ID", "Id" }, { "MaxHpLv", "MaxHp" }, { "MaxMpLv", "MaxMp" }, { "EXP", "Exp" },
                                                 { "TM", "Tm" }, { "ApLv", "Ap" }, { "AcLv", "Ac" }, { "DxLv", "Dx" }, { "MaLv", "Ma" },
                                                 { "MdLv", "Md" }, { "DaLv", "Da" }, { "LkLv", "Lk" }, { "DpLv", "Dp" }, { "HvLv", "Hv" },
                                                 { "MaxWtLv", "MaxWt" } };
        for (YRecord& r : out)
            for (const auto& a : kAlias)
                if (const std::string* v = r.get(a[0])) r.scalars[a[1]] = *v;
        return true;
    }
    if (db == "maps_db") { out = map_rows(); if (out.empty()) { err = "no MapInfoEx in " + lc.path(); return false; } return true; }
    if (db == "skill_db") return one("SkillParam2");
    if (db == "gun_db") return one("Item_GunParam2");
    if (db == "drill_db") return one("Item_DrillParam");
    if (db == "drill_grade_exp_db") return one("DrillGradeExpRate");
    if (db == "map_attr_db") return one("MapAttr");
    if (db == "monster_quest_db") return one("MonsterQuestInfo");
    if (db == "quest_db") return one("QuestInfo");
    if (db == "scenario_quest_db") return one("ScenarioQuestInfo");
    if (db == "npc_check_lv_msg_db") return one("NpcCheckLvMsg");
    if (db == "teleport_db") return one("TeleportInfo");
    if (db == "npc_move_db") return one("NpcMoveInfo");
    if (db == "random_port_db") return one("RandomPortParam");
    if (db == "teleport_adv_db") return one("TeleportAdvInfo");
    if (db == "exchange_shop_db") return one("ExchangeShopContents");
    if (db == "mon_item_drop_db") return one("MonItemDropInfo");
    if (db == "mon_item_table_db") return one("MonItemTable_DropInfo");
    if (db == "map_item_event_add_db") return one("MapItem_Event_Add");
    if (db == "map_item_table_db") return one("MapItem_ETC_Table");
    if (db == "map_item_db") return one("MapItem_All");
    if (db == "compound_db") {                              // the three compound tables (Compound_Test is not read)
        for (const char* t : { "Compound_Potion", "Compound_Rare", "Compound_Throw" }) lc.rows(t, out, "Table");
        return true;
    }
    if (db == "compounder_db") {                            // Compounder_Info's lists, each list's rows + its name and Grade
        std::vector<YRecord> info;
        lc.rows("Compounder_Info", info);
        std::set<std::string> seen;
        for (const YRecord& li : info) {
            const std::string name = li.str("ListName");
            if (name.empty() || !seen.insert(name).second) continue;
            const size_t from = out.size();
            lc.rows(name, out, "ListName");
            for (size_t k = from; k < out.size(); ++k)
                if (const std::string* g = li.get("Grade")) out[k].scalars["Grade"] = *g;
        }
        return true;
    }
    if (db == "esa_db") {                                   // the ESAII / ESAUI / ESAction tables the skills name
        std::vector<YRecord> sk;
        lc.rows("SkillParam2", sk);
        std::vector<std::string> names;
        std::set<std::string> seen;
        for (const char* f : { "ESAIITable", "ESAUITable", "ESATable" }) refs(sk, f, names, seen);
        return tables(names);
    }
    if (db == "quest_result_db" || db == "monster_quest_result_db") {   // the result tables the quests name
        std::vector<YRecord> qs;
        if (db == "quest_result_db") { lc.rows("QuestInfo", qs); lc.rows("ScenarioQuestInfo", qs); }
        else lc.rows("MonsterQuestInfo", qs);
        std::vector<std::string> names;
        std::set<std::string> seen;
        refs(qs, "ResultTable", names, seen);
        return tables(names);
    }
    if (db == "mon_spawn_db") {                             // the Mon_ tables the maps name
        std::vector<std::string> names;
        std::set<std::string> seen;
        refs(map_rows(), "MonsterTable", names, seen);
        return tables(names);
    }
    if (db == "npc_db") {                                   // the NpcTalk tables the maps name, + the CharacterInfo name
        std::vector<std::string> names;
        npc_tables(names);
        tables(names);
        const auto cn = char_names();
        for (YRecord& r : out) {
            auto c = cn.find(r.num("Type"));
            if (c != cn.end() && !c->second.empty()) r.scalars["Name"] = c->second;
        }
        return true;
    }
    if (db == "shop_db" || db == "shop_item_db") {          // the Shop tables the maps name (+ name), their item tables
        std::vector<std::string> names;
        std::set<std::string> seen;
        refs(map_rows(), "ShopTable", names, seen);
        if (db == "shop_db") {
            tables(names);
            const auto cn = char_names();
            for (YRecord& r : out) {
                auto c = cn.find(r.num("NpcType"));
                if (c != cn.end() && !c->second.empty()) r.scalars["Name"] = c->second;
            }
            return true;
        }
        std::vector<YRecord> shops;
        for (const std::string& n : names) lc.rows(n, shops);
        std::vector<std::string> items;
        std::set<std::string> seen2;
        refs(shops, "ItemTable", items, seen2);
        return tables(items);
    }
    if (db == "exchange_shop_table_db" || db == "npc_msg_db") {   // the tables the NPCs name (+ every NpcMsg_ table)
        std::vector<std::string> npcT;
        npc_tables(npcT);
        std::vector<YRecord> npcs;
        for (const std::string& n : npcT) lc.rows(n, npcs);
        std::vector<std::string> names;
        std::set<std::string> seen;
        if (db == "npc_msg_db")
            for (const std::string& n : lc.with_prefix("NpcMsg_")) if (seen.insert(n).second) names.push_back(n);
        refs(npcs, db == "npc_msg_db" ? "MsgTable" : "ExcShopTable", names, seen);
        return tables(names);
    }
    err = "the libconfig reader does not know " + db;
    return false;
}

} // namespace nj
