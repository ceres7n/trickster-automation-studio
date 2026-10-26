#pragma once
// Packet definitions (data/packets_db.yml + the data/packets_neo.yml overlay) and the
// field decoder the inspector uses.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nj {

struct FieldSpec {
    enum class Kind { U8, U16, U32, U64, I8, I16, I32, I64, F32, Str, FixedStr, Blob, Tuple, Rest, Loop };
    enum class Count { Previous, Literal, UntilEnd };

    Kind        kind = Kind::U8;
    uint32_t    size = 0;            // FixedStr / Blob
    std::string type;                // as written
    std::string name;
    Count       count = Count::Previous;   // Loop
    uint32_t    literal = 0;
    std::vector<FieldSpec> children;
};

struct PacketDef {
    uint16_t    id = 0;
    uint8_t     dir = 0;             // PacketDir
    std::string name, handler, note, source;
    std::vector<std::string> fieldsRaw;
    std::vector<FieldSpec>   fields;
    std::string fieldError;          // the Fields list did not parse
};

struct DecodedRow {
    int         depth = 0;
    std::string label, type, value;
    uint32_t    off = 0, len = 0;
    bool        error = false;
    bool        trailing = false;
};

class PacketDb {
public:
    // Later files override earlier ones per Dir + Id.
    bool load(const std::string& path, std::string& err);
    void clear() { defs_.clear(); files_.clear(); }

    const PacketDef* find(uint8_t dir, uint16_t op) const;
    size_t size() const { return defs_.size(); }
    const std::vector<std::string>& files() const { return files_; }

    // Decodes the payload against def.fields. Leftover bytes become a "trailing" row.
    void decode(const PacketDef& def, const uint8_t* p, size_t n, std::vector<DecodedRow>& rows) const;

    // True when every field of `def` is a flat scalar/string (no loops/tuples/blobs) and so can
    // be built from one value string per field in the Send tab's field editor.
    static bool editable_fields(const PacketDef& def);
    // Builds the payload from `values` (one per def.fields entry) — numbers decimal or 0x-hex,
    // strings as CP949 text, blobN/rest as hex. On error returns false and sets `err` + `badField`.
    static bool encode(const PacketDef& def, const std::vector<std::string>& values, std::vector<uint8_t>& out,
                       std::string& err, int& badField);

private:
    std::map<uint32_t, PacketDef> defs_;   // dir << 16 | id
    std::vector<std::string> files_;
};

} // namespace nj
