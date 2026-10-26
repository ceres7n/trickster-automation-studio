#pragma once
// ── Story (act_story.cpp): the scenario quests, chapter by chapter (libconfig ScenarioQuestEpisode: episodes 0..6, each
//    chapter from its first scenario quest to its last). The plan: each chapter's last quest, from the first chapter
//    chosen (or the first not known done) to the target one — and what its NPCs' dialogs put before it (the chapter's
//    other quests, a monster quest, a quest whose reward is an item it needs). A scenario quest is taken as soon as it
//    is ready (a map or an NPC may wait for it), then its items are got (kill / dig, another quest's reward, an NPC's
//    Give_Item). The runner (quest_runner.h) does the rest. On, it turns the Key Quest, the Monster Quest, the Item Hunt
//    and the Drill off and the Attack on. ──
#include "app/bot/quest_runner.h"

namespace nj {

class StoryAction : public QuestRunner {
public:
    const char* key() const override { return "story"; }
    const char* title() const override { return "Story"; }
    void save(std::ostream& o) const override;
    void load(const std::string& k, const std::string& v) override;

    int fromChapter = -1;               // QuestGraph::chapters() index to start at (-1 = the first not known done)
    int toChapter = -1;                 // the last chapter to do (-1 = none chosen)
    // the first chapter the plan starts at now (fromChapter, or the first whose last quest is not known done)
    int start_chapter(const WorldModel& w, const QuestBook& book) const;

protected:
    void plan_own(const WorldModel& w, const View& v, std::vector<QJob>& jobs) const override;
    std::string plan_key() const override;
    std::string done_text() const override;
    std::string idle_text() const override { return "choose the chapter to reach (Story Quest tab)"; }
    bool configured(const WorldModel& w) const override;
    const char* label() const override { return "story"; }
};

} // namespace nj
