#pragma once
// The YAML subset of the rAthena-style db files (Header / Body / Footer): the Body is a
// list of flat records whose values are scalars ('quoted', "quoted", bare) or one-line
// flow lists ['a', 'b']. Only the record level is read; nested keys/lists are skipped.
#include <istream>
#include <map>
#include <string>
#include <vector>

namespace nj {

struct YRecord {
    int line = 0;
    std::map<std::string, std::string> scalars;
    std::map<std::string, std::vector<std::string>> lists;

    const std::string* get(const char* key) const;
    std::string str(const char* key, const std::string& def = std::string()) const;
    long long   num(const char* key, long long def = 0) const;
    double      fnum(const char* key, double def = 0.0) const;   // 0.5, 1e3, ...
};

std::string yml_trim(const std::string& s);
bool yml_read_body(std::istream& in, std::vector<YRecord>& out, std::string& err);
bool yml_read_body_file(const std::string& path, std::vector<YRecord>& out, std::string& err);

} // namespace nj
