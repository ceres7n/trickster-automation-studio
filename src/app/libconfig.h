#pragma once
// The client's libconfig XML (conf\paula_libconfig.xml): <INI><TABLE name=".." RowCount=".."><FIELDINFO Name=".."
// IsKey=".." DataType="INT|float|String|Enum|bool"/>...<ROW><Field>value</Field>...</ROW></TABLE>...</INI>, 10460 tables.
// The game data is read from it the way the ProjectPAULA db/*.yml files were made from the server's libconfig: each db
// is one table (or the tables another one names), its rows in order, field names verbatim (libconfig_db).
#include "app/yml_lite.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace nj {

class LibConfig {
public:
    bool load(const std::string& path, std::string& err);     // reads the file and indexes its tables
    bool loaded() const { return !text_.empty(); }
    const std::string& path() const { return path_; }
    size_t tables() const { return order_.size(); }
    bool has(const std::string& table) const { return index_.count(table) != 0; }
    // A table's rows as records, in order: CDATA unwrapped, entities decoded, an Enum list "2/32" -> its OR (34, as the
    // retail loader's strtok '/'), and - the yml files' rule - an empty value or a numeric zero left out. `tableField`:
    // each record also gets that field = the table's name (the yml files' "Table"). false = no such table.
    bool rows(const std::string& table, std::vector<YRecord>& out, const char* tableField = nullptr) const;
    std::vector<std::string> with_prefix(const std::string& prefix) const;   // table names, in file order

private:
    struct Span { size_t begin = 0, end = 0; };
    std::string text_, path_;
    std::unordered_map<std::string, Span> index_;
    std::vector<std::string> order_;
};

// The records of one ProjectPAULA db file name ("item_db", "npc_db", ...) made from the libconfig: the same fields the
// yml file has (item_db = ItemParam2 + ItemParamEx2 + ItemParamCM2 + ItemParamEx; mob_db = MonsterParamEx2 with the yml's
// names; npc_db / shop_db / mon_spawn_db = the tables MapInfoEx names, + "Table" and the CharacterInfo name; ...).
// false = a db this reader does not know (`err` says so).
bool libconfig_db(const LibConfig& lc, const std::string& db, std::vector<YRecord>& out, std::string& err);

} // namespace nj
