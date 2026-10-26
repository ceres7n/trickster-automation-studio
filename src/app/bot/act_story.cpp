// Story (act_story.h): the plan — each chapter's last scenario quest from the start chapter to the target one; the
// runner (quest_runner.cpp) adds what the dialogs put before each and does the rest.
#include "app/bot/act_story.h"
#include "app/bot/bot_util.h"
#include "app/game_data.h"
#include "app/text_util.h"

namespace nj {

int StoryAction::start_chapter(const WorldModel& w, const QuestBook& book) const
{
    const GameData* gd = w.data();
    if (!gd) return -1;
    const std::vector<QuestGraph::Chapter>& chs = gd->quest_graph().chapters();
    if (fromChapter >= 0 && fromChapter < int(chs.size())) return fromChapter;
    const View v(book, w);
    for (size_t i = 0; i < chs.size(); ++i)
        if (v.hist_lo(QKey{ false, chs[i].last, true }) < 1) return int(i);
    return int(chs.size());
}

void StoryAction::plan_own(const WorldModel& w, const View& v, std::vector<QJob>& jobs) const
{
    const QuestGraph& g = w.data()->quest_graph();
    const std::vector<QuestGraph::Chapter>& chs = g.chapters();
    if (toChapter < 0 || toChapter >= int(chs.size())) return;
    int from = fromChapter;
    if (from < 0) {                                     // the first chapter whose last quest is not known done
        from = int(chs.size());
        for (size_t i = 0; i < chs.size(); ++i)
            if (v.hist_lo(QKey{ false, chs[i].last, true }) < 1) { from = int(i); break; }
    }
    // the chapters' quests come one after the other: a quest of them whose history is not known is taken as not done, so
    // the plan starts at the chapter's first quest and goes on in order (a live log: the bot walked to a chapter's last
    // quest, learned the one before was not done, walked there, ... - and tried other classes' variants for ever)
    linear_.on = true;
    linear_.before.clear();
    for (int i = 0; i < from && i < int(chs.size()); ++i) {
        for (int q : chs[size_t(i)].quests) linear_.before.insert(QKey{ false, q, true }.v());
        linear_.before.insert(QKey{ false, chs[size_t(i)].first, true }.v());
        linear_.before.insert(QKey{ false, chs[size_t(i)].last, true }.v());
    }
    for (int i = from; i <= toChapter && i < int(chs.size()); ++i) g.expand(QKey{ false, chs[size_t(i)].last, true }, 1, v, jobs);
}

std::string StoryAction::plan_key() const { return fmt("%d|%d", fromChapter, toChapter); }

std::string StoryAction::done_text() const { return "story: the chapters chosen are done"; }

bool StoryAction::configured(const WorldModel& w) const
{
    return w.data() && toChapter >= 0 && toChapter < int(w.data()->quest_graph().chapters().size());
}

void StoryAction::save(std::ostream& o) const
{
    botu::kv(o, key(), "from", fromChapter);
    botu::kv(o, key(), "to", toChapter);
    botu::kv(o, key(), "usePortal", usePortal);
    save_acquire(o, key());
}

void StoryAction::load(const std::string& k, const std::string& v)
{
    if (k == "from") fromChapter = int(botu::num(v));
    else if (k == "to") toChapter = int(botu::num(v));
    else if (k == "usePortal") usePortal = botu::flag(v);
    else load_acquire(k, v);
}

} // namespace nj
