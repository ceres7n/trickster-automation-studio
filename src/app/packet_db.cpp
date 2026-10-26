#include "app/packet_db.h"
#include "app/text_util.h"
#include "app/yml_lite.h"
#include "shared/ipc.h"
#include <cstring>

namespace nj {

namespace {

// ── field specs ─────────────────────────────────────────────────────────────
std::vector<std::string> split_top(const std::string& s)
{
    std::vector<std::string> parts;
    int depth = 0;
    std::string cur;
    for (char c : s) {
        if (c == '{') ++depth;
        if (c == '}') --depth;
        if (c == ',' && depth == 0) { parts.push_back(yml_trim(cur)); cur.clear(); continue; }
        cur += c;
    }
    if (!yml_trim(cur).empty()) parts.push_back(yml_trim(cur));
    return parts;
}

bool parse_field(const std::string& tok, FieldSpec& f, std::string& err)
{
    std::string s = yml_trim(tok);
    size_t star = s.find("*{");
    if (star != std::string::npos) {
        size_t close = s.rfind('}');
        if (close == std::string::npos || close < star) { err = "unclosed loop: " + s; return false; }
        std::string prefix = yml_trim(s.substr(0, star));
        f.kind = FieldSpec::Kind::Loop;
        f.type = s.substr(0, close + 1);
        f.name = yml_trim(s.substr(close + 1));
        uint32_t lit = 0;
        if (prefix == "N" || prefix.empty()) f.count = FieldSpec::Count::Previous;
        else if (prefix == "rest" || prefix == "*") f.count = FieldSpec::Count::UntilEnd;
        else if (parse_uint(prefix, lit)) { f.count = FieldSpec::Count::Literal; f.literal = lit; }
        else { err = "bad loop count: " + prefix; return false; }
        for (const std::string& c : split_top(s.substr(star + 2, close - star - 2))) {
            FieldSpec child;
            if (!parse_field(c, child, err)) return false;
            f.children.push_back(std::move(child));
        }
        return true;
    }

    size_t sp = s.find(' ');
    std::string type = sp == std::string::npos ? s : s.substr(0, sp);
    f.name = sp == std::string::npos ? "" : yml_trim(s.substr(sp + 1));
    f.type = type;
    using K = FieldSpec::Kind;
    static const std::map<std::string, K> simple = {
        { "u8", K::U8 }, { "u16", K::U16 }, { "u32", K::U32 }, { "u64", K::U64 },
        { "i8", K::I8 }, { "i16", K::I16 }, { "i32", K::I32 }, { "i64", K::I64 },
        { "f32", K::F32 }, { "str", K::Str }, { "tuple", K::Tuple }, { "rest", K::Rest },
    };
    auto it = simple.find(type);
    if (it != simple.end()) { f.kind = it->second; return true; }
    uint32_t n = 0;
    if (type == "equipset234") { f.kind = K::Blob; f.size = 234; return true; }
    if (type.rfind("blob", 0) == 0 && parse_uint(type.substr(4), n)) { f.kind = K::Blob; f.size = n; return true; }
    if (type.rfind("str", 0) == 0 && parse_uint(type.substr(3), n)) { f.kind = K::FixedStr; f.size = n; return true; }
    err = "unknown field type: " + type;
    return false;
}

// ── decoding ────────────────────────────────────────────────────────────────
constexpr size_t kMaxRows = 4000;

struct Cursor {
    const uint8_t* p;
    size_t n;
    size_t off;
    size_t left() const { return n - off; }
};

uint64_t read_le(const uint8_t* p, int size)
{
    uint64_t v = 0;
    for (int i = size - 1; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

bool decode_list(const std::vector<FieldSpec>& fs, Cursor& c, int depth, std::vector<DecodedRow>& rows);

bool need(Cursor& c, size_t bytes, const FieldSpec& f, int depth, std::vector<DecodedRow>& rows)
{
    if (c.left() >= bytes) return true;
    DecodedRow r;
    r.depth = depth;
    r.label = f.name;
    r.type = f.type;
    r.off = (uint32_t)c.off;
    r.value = fmt("needs %zu bytes, %zu left", bytes, c.left());
    r.error = true;
    rows.push_back(std::move(r));
    return false;
}

bool decode_one(const FieldSpec& f, Cursor& c, int depth, std::vector<DecodedRow>& rows, uint64_t& lastInt)
{
    using K = FieldSpec::Kind;
    DecodedRow r;
    r.depth = depth;
    r.label = f.name;
    r.type = f.type;
    r.off = (uint32_t)c.off;

    int isz = 0;
    bool sgn = false;
    switch (f.kind) {
    case K::U8:  isz = 1; break;
    case K::U16: isz = 2; break;
    case K::U32: isz = 4; break;
    case K::U64: isz = 8; break;
    case K::I8:  isz = 1; sgn = true; break;
    case K::I16: isz = 2; sgn = true; break;
    case K::I32: isz = 4; sgn = true; break;
    case K::I64: isz = 8; sgn = true; break;
    default: break;
    }
    if (isz) {
        if (!need(c, isz, f, depth, rows)) return false;
        uint64_t v = read_le(c.p + c.off, isz);
        if (sgn) {
            int64_t sv = (int64_t)(v << (64 - 8 * isz)) >> (64 - 8 * isz);
            r.value = fmt("%lld", (long long)sv);
            lastInt = sv < 0 ? 0 : (uint64_t)sv;
        } else {
            r.value = fmt("%llu  (0x%0*llX)", (unsigned long long)v, isz * 2, (unsigned long long)v);
            lastInt = v;
        }
        r.len = isz;
        c.off += isz;
        rows.push_back(std::move(r));
        return true;
    }

    switch (f.kind) {
    case K::F32: {
        if (!need(c, 4, f, depth, rows)) return false;
        float v;
        memcpy(&v, c.p + c.off, 4);
        r.value = fmt("%g", v);
        r.len = 4;
        break;
    }
    case K::Str: {
        const void* z = memchr(c.p + c.off, 0, c.left());
        if (!z) {
            r.value = "unterminated: \"" + cp949_to_utf8((const char*)c.p + c.off, c.left()) + "\"";
            r.len = (uint32_t)c.left();
            r.error = true;
            rows.push_back(std::move(r));
            return false;
        }
        size_t len = (const uint8_t*)z - (c.p + c.off);
        r.value = "\"" + cp949_to_utf8((const char*)c.p + c.off, len) + "\"";
        r.len = (uint32_t)len + 1;
        break;
    }
    case K::FixedStr: {
        if (!need(c, f.size, f, depth, rows)) return false;
        const void* z = memchr(c.p + c.off, 0, f.size);
        size_t len = z ? (const uint8_t*)z - (c.p + c.off) : f.size;
        r.value = "\"" + cp949_to_utf8((const char*)c.p + c.off, len) + "\"";
        r.len = f.size;
        break;
    }
    case K::Blob:
        if (!need(c, f.size, f, depth, rows)) return false;
        r.value = hex_preview(c.p + c.off, f.size, 32);
        r.len = f.size;
        break;
    case K::Rest:
        r.value = c.left() ? fmt("%zu bytes: ", c.left()) + hex_preview(c.p + c.off, c.left(), 32) : "(none)";
        r.len = (uint32_t)c.left();
        break;
    case K::Tuple: {
        if (!need(c, 7, f, depth, rows)) return false;
        r.value = fmt("(%u, %u, %u)", (unsigned)read_le(c.p + c.off, 4), (unsigned)read_le(c.p + c.off + 4, 2),
                      (unsigned)c.p[c.off + 6]);
        r.len = 7;
        break;
    }
    case K::Loop: {
        uint64_t count = f.count == FieldSpec::Count::Previous ? lastInt
                       : f.count == FieldSpec::Count::Literal  ? f.literal : 0;
        const bool untilEnd = f.count == FieldSpec::Count::UntilEnd;
        r.label = f.name.empty() ? "loop" : f.name;
        r.value = untilEnd ? "until the end" : fmt("%llu entries", (unsigned long long)count);
        rows.push_back(r);
        for (uint64_t i = 0; untilEnd ? c.left() > 0 : i < count; ++i) {
            if (rows.size() >= kMaxRows) return false;
            DecodedRow hdr;
            hdr.depth = depth + 1;
            hdr.label = fmt("[%llu]", (unsigned long long)i);
            hdr.off = (uint32_t)c.off;
            rows.push_back(hdr);
            size_t before = c.off;
            if (!decode_list(f.children, c, depth + 2, rows)) return false;
            if (c.off == before) break;   // an empty iteration would loop forever
        }
        return true;
    }
    default:
        break;
    }
    c.off += r.len;
    rows.push_back(std::move(r));
    return true;
}

bool decode_list(const std::vector<FieldSpec>& fs, Cursor& c, int depth, std::vector<DecodedRow>& rows)
{
    uint64_t lastInt = 0;
    for (const FieldSpec& f : fs) {
        if (rows.size() >= kMaxRows) {
            DecodedRow r;
            r.depth = depth;
            r.label = "(row limit)";
            r.error = true;
            rows.push_back(std::move(r));
            return false;
        }
        if (!decode_one(f, c, depth, rows, lastInt)) return false;
    }
    return true;
}

} // namespace

bool PacketDb::load(const std::string& path, std::string& err)
{
    std::vector<YRecord> recs;
    if (!yml_read_body_file(path, recs, err)) return false;

    const std::string file = path.substr(path.find_last_of("\\/") + 1);
    for (YRecord& y : recs) {
        uint32_t id = 0;
        auto idIt = y.scalars.find("Id");
        auto dirIt = y.scalars.find("Dir");
        if (idIt == y.scalars.end() || dirIt == y.scalars.end() || !parse_uint(idIt->second, id) || id > 0xFFFF)
            continue;
        const uint8_t dir = dirIt->second == "S2C" ? DIR_S2C : DIR_C2S;
        const uint32_t key = uint32_t(dir) << 16 | id;

        bool fresh = defs_.find(key) == defs_.end();
        PacketDef& d = defs_[key];
        // packets_db.yml repeats a few ids (one handler per screen): keep the first.
        if (!fresh && d.source == file) continue;
        d.id = (uint16_t)id;
        d.dir = dir;
        auto get = [&](const char* k, std::string& out) {
            auto it = y.scalars.find(k);
            if (it != y.scalars.end() && !it->second.empty()) out = it->second;
        };
        get("Name", d.name);
        get("Handler", d.handler);
        get("Note", d.note);
        d.source = file;
        get("Source", d.source);
        if (d.source != file) d.source = file + ": " + d.source;

        auto fl = y.lists.find("Fields");
        if (fl != y.lists.end() && !fl->second.empty()) {
            d.fieldsRaw = fl->second;
            d.fields.clear();
            d.fieldError.clear();
            for (const std::string& tok : fl->second) {
                FieldSpec f;
                if (!parse_field(tok, f, d.fieldError)) { d.fields.clear(); break; }
                d.fields.push_back(std::move(f));
            }
        }
    }
    files_.push_back(path);
    return true;
}

bool PacketDb::editable_fields(const PacketDef& def)
{
    if (def.fields.empty() || !def.fieldError.empty()) return false;
    using K = FieldSpec::Kind;
    for (const FieldSpec& f : def.fields)
        if (f.kind == K::Loop || f.kind == K::Tuple) return false;
    return true;
}

bool PacketDb::encode(const PacketDef& def, const std::vector<std::string>& values, std::vector<uint8_t>& out,
                      std::string& err, int& badField)
{
    using K = FieldSpec::Kind;
    out.clear();
    badField = -1;
    auto put_le = [&](uint64_t v, int n) { for (int i = 0; i < n; ++i) out.push_back((uint8_t)(v >> (8 * i))); };

    for (size_t i = 0; i < def.fields.size(); ++i) {
        const FieldSpec& f = def.fields[i];
        const std::string val = i < values.size() ? values[i] : std::string();
        badField = (int)i;
        int isz = 0;
        bool sgn = false;
        switch (f.kind) {
        case K::U8:  isz = 1; break;
        case K::U16: isz = 2; break;
        case K::U32: isz = 4; break;
        case K::U64: isz = 8; break;
        case K::I8:  isz = 1; sgn = true; break;
        case K::I16: isz = 2; sgn = true; break;
        case K::I32: isz = 4; sgn = true; break;
        case K::I64: isz = 8; sgn = true; break;
        default: break;
        }
        if (isz) {
            if (sgn) {
                int64_t v = 0;
                if (!val.empty() && !parse_int64(val, v)) { err = f.type + ": not a number"; return false; }
                put_le((uint64_t)v, isz);
            } else {
                uint64_t v = 0;
                if (!val.empty() && !parse_uint64(val, v)) { err = f.type + ": not a number"; return false; }
                put_le(v, isz);
            }
            continue;
        }
        switch (f.kind) {
        case K::F32: {
            float v = 0;
            if (!val.empty()) {
                char* e = nullptr;
                v = strtof(val.c_str(), &e);
                if (!e || *e) { err = "f32: not a number"; return false; }
            }
            uint32_t bits;
            memcpy(&bits, &v, 4);
            put_le(bits, 4);
            break;
        }
        case K::Str: {
            std::string c = utf8_to_cp949(val);
            out.insert(out.end(), c.begin(), c.end());
            out.push_back(0);
            break;
        }
        case K::FixedStr: {
            std::string c = utf8_to_cp949(val);
            c.resize(f.size, '\0');
            out.insert(out.end(), c.begin(), c.end());
            break;
        }
        case K::Blob:
        case K::Rest: {
            std::vector<uint8_t> b;
            std::string he;
            if (!parse_hex(val, b, he)) { err = f.type + ": " + he; return false; }
            if (f.kind == K::Blob && b.size() != f.size) {
                err = fmt("%s: need exactly %u bytes, got %zu", f.type.c_str(), f.size, b.size());
                return false;
            }
            out.insert(out.end(), b.begin(), b.end());
            break;
        }
        default:
            err = "field type not supported in the editor: " + f.type;
            return false;
        }
    }
    badField = -1;
    return true;
}

const PacketDef* PacketDb::find(uint8_t dir, uint16_t op) const
{
    auto it = defs_.find(uint32_t(dir) << 16 | op);
    return it == defs_.end() ? nullptr : &it->second;
}

void PacketDb::decode(const PacketDef& def, const uint8_t* p, size_t n, std::vector<DecodedRow>& rows) const
{
    Cursor c{ p, n, 0 };
    if (!decode_list(def.fields, c, 0, rows)) return;
    if (c.left()) {
        DecodedRow r;
        r.label = "(trailing)";
        r.off = (uint32_t)c.off;
        r.len = (uint32_t)c.left();
        r.value = fmt("%zu bytes: ", c.left()) + hex_preview(p + c.off, c.left(), 32);
        r.trailing = true;
        rows.push_back(std::move(r));
    }
}

} // namespace nj
