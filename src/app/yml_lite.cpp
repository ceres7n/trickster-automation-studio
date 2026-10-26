#include "app/yml_lite.h"
#include "app/text_util.h"
#include <cstdlib>
#include <fstream>

namespace nj {

namespace {

// One scalar starting at s[i]; i ends past it.
std::string read_scalar(const std::string& s, size_t& i, bool inList)
{
    if (i < s.size() && s[i] == '\'') {
        std::string out;
        for (++i; i < s.size(); ++i) {
            if (s[i] == '\'') {
                if (i + 1 < s.size() && s[i + 1] == '\'') { out += '\''; ++i; continue; }
                ++i;
                break;
            }
            out += s[i];
        }
        return out;
    }
    if (i < s.size() && s[i] == '"') {
        std::string out;
        for (++i; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) { out += s[++i]; continue; }
            if (s[i] == '"') { ++i; break; }
            out += s[i];
        }
        return out;
    }
    size_t start = i;
    while (i < s.size() && !(inList && (s[i] == ',' || s[i] == ']')) &&
           !(s[i] == '#' && i > start && s[i - 1] == ' '))
        ++i;
    return yml_trim(s.substr(start, i - start));
}

bool parse_flow_list(const std::string& v, std::vector<std::string>& out)
{
    size_t i = 1;   // past '['
    for (;;) {
        while (i < v.size() && (v[i] == ' ' || v[i] == '\t')) ++i;
        if (i >= v.size()) return false;
        if (v[i] == ']') return true;
        out.push_back(read_scalar(v, i, true));
        while (i < v.size() && (v[i] == ' ' || v[i] == '\t')) ++i;
        if (i < v.size() && v[i] == ',') { ++i; continue; }
        if (i < v.size() && v[i] == ']') return true;
        return false;
    }
}

} // namespace

const std::string* YRecord::get(const char* key) const
{
    auto it = scalars.find(key);
    return it == scalars.end() ? nullptr : &it->second;
}

std::string YRecord::str(const char* key, const std::string& def) const
{
    const std::string* v = get(key);
    return v ? *v : def;
}

long long YRecord::num(const char* key, long long def) const
{
    const std::string* v = get(key);
    if (!v || v->empty()) return def;
    char* end = nullptr;
    long long r = strtoll(v->c_str(), &end, 0);
    return end && end != v->c_str() ? r : def;
}

double YRecord::fnum(const char* key, double def) const
{
    const std::string* v = get(key);
    if (!v || v->empty()) return def;
    char* end = nullptr;
    double r = strtod(v->c_str(), &end);
    return end && end != v->c_str() ? r : def;
}

std::string yml_trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

bool yml_read_body(std::istream& in, std::vector<YRecord>& out, std::string& err)
{
    std::string line;
    int lineNo = 0;
    bool inBody = false;
    size_t recIndent = std::string::npos;   // indent of the "- " that starts a record
    YRecord cur;
    bool have = false;
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = yml_trim(line);
        if (t.empty() || t[0] == '#') continue;
        size_t indent = line.find_first_not_of(' ');
        if (indent == 0) {
            if (have) { out.push_back(std::move(cur)); cur = YRecord(); have = false; }
            inBody = t.rfind("Body:", 0) == 0;
            continue;
        }
        if (!inBody) continue;
        if (t.rfind("- ", 0) == 0) {
            if (recIndent == std::string::npos) recIndent = indent;
            if (indent > recIndent) continue;          // an item of a nested list
            if (have) out.push_back(std::move(cur));
            cur = YRecord();
            cur.line = lineNo;
            have = true;
            t = yml_trim(t.substr(2));
        } else if (recIndent != std::string::npos && indent > recIndent + 2) {
            continue;                                  // a nested key
        }
        if (!have) continue;
        size_t colon = t.find(':');
        if (colon == std::string::npos) {
            err = fmt("line %d: expected 'Key: value'", lineNo);
            return false;
        }
        std::string key = yml_trim(t.substr(0, colon));
        std::string val = yml_trim(t.substr(colon + 1));
        if (!val.empty() && val[0] == '[') {
            std::vector<std::string> items;
            if (!parse_flow_list(val, items)) {
                err = fmt("line %d: bad list", lineNo);
                return false;
            }
            cur.lists[key] = std::move(items);
        } else {
            size_t i = 0;
            cur.scalars[key] = read_scalar(val, i, false);
        }
    }
    if (have) out.push_back(std::move(cur));
    return true;
}

bool yml_read_body_file(const std::string& path, std::vector<YRecord>& out, std::string& err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open " + path; return false; }
    if (!yml_read_body(in, out, err)) { err = path + ": " + err; return false; }
    return true;
}

} // namespace nj
