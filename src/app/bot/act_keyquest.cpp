// Key Quest (act_keyquest.h): the plan of a town — its picked sticker quests to their sticker's run, then (doKey) the
// key quest and its follow-ups at its NPC. The runner (quest_runner.cpp) does the rest.
#include "app/bot/act_keyquest.h"
#include "app/bot/bot_util.h"
#include "app/game_data.h"
#include "app/text_util.h"
#include <algorithm>

namespace nj {

void KeyQuestAction::plan_own(const WorldModel& w, const View& v, std::vector<QJob>& jobs) const
{
    const QuestGraph& g = w.data()->quest_graph();
    const KeyTown* t = g.town(sticker);
    if (!t) return;
    // the lower levels first (Swamp: the level-140 quests, then the 150 ones)
    std::vector<const StickerQuest*> order;
    for (const StickerQuest& sq : t->quests)
        if (picked.count(sq.q.id)) order.push_back(&sq);
    auto lv = [&](const StickerQuest* sq) { const QuestInfo* qi = g.info(sq->q); return qi ? qi->minLevel : 0; };
    std::stable_sort(order.begin(), order.end(), [&](const StickerQuest* a, const StickerQuest* b) { return lv(a) < lv(b); });
    for (const StickerQuest* sq : order) g.expand(sq->q, sq->runs, v, jobs);
    if (doKey)
        for (const QKey& k : t->chain) g.expand(k, 1, v, jobs);
}

std::string KeyQuestAction::plan_key() const
{
    std::string key = fmt("%u|%d|", sticker, doKey ? 1 : 0);
    for (int q : picked) key += fmt("%d,", q);
    return key;
}

void KeyQuestAction::keep_items(const WorldModel&, std::vector<uint32_t>& items) const
{
    if (sticker) items.push_back(sticker);              // (never sold, the key quest done or not)
}

std::string KeyQuestAction::done_text() const
{
    return "key quest: everything picked is done";
}

std::string KeyQuestAction::idle_text() const
{
    return sticker ? "pick the sticker quests to do (Key Quest tab)" : "choose a town (Key Quest tab)";
}

bool KeyQuestAction::configured(const WorldModel& w) const
{
    return w.data() && w.data()->quest_graph().town(sticker) && (!picked.empty() || doKey);
}

void KeyQuestAction::save(std::ostream& o) const
{
    botu::kv(o, key(), "sticker", sticker);
    botu::kv(o, key(), "doKey", doKey);
    botu::kv(o, key(), "usePortal", usePortal);
    save_acquire(o, key());
    for (int q : picked) botu::kv(o, key(), "pick", q);
}

void KeyQuestAction::load(const std::string& k, const std::string& v)
{
    if (k == "sticker") sticker = uint32_t(botu::num(v));
    else if (k == "doKey") doKey = botu::flag(v);
    else if (k == "usePortal") usePortal = botu::flag(v);
    else if (k == "pick") { const int q = int(botu::num(v)); if (q > 0) picked.insert(q); }
    else load_acquire(k, v);
}

} // namespace nj
