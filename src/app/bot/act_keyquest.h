#pragma once
// ── Key Quest (act_keyquest.cpp): a town's Key Quest. The town's sticker quests the user picks (each one done until
//    the run whose result gives the sticker — usually its 5th), then the key quest itself and its follow-ups at the same
//    NPC (unless "only complete sticker quests"). The runner (quest_runner.h) takes and hands in each quest through its
//    NPC's dialog, does first what the dialogs put before it (a chain, a monster quest to kill for) and gets the items.
//    On, it turns the Story, the Monster Quest, the Item Hunt and the Drill off and the Attack on. ──
#include "app/bot/quest_runner.h"

namespace nj {

class KeyQuestAction : public QuestRunner {
public:
    const char* key() const override { return "keyq"; }
    const char* title() const override { return "Key Quest"; }
    void save(std::ostream& o) const override;
    void load(const std::string& k, const std::string& v) override;

    uint32_t sticker = 0;               // the town: its sticker item
    std::set<int> picked;               // the town's sticker quests to do (quest ids)
    bool doKey = true;                  // then the key quest and its follow-ups

protected:
    void plan_own(const WorldModel& w, const View& v, std::vector<QJob>& jobs) const override;
    std::string plan_key() const override;
    void keep_items(const WorldModel& w, std::vector<uint32_t>& items) const override;
    std::string done_text() const override;
    std::string idle_text() const override;
    bool configured(const WorldModel& w) const override;
    const char* label() const override { return "key quest"; }
};

} // namespace nj
