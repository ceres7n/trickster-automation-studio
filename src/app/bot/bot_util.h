#pragma once
// Small helpers shared by the bot's actions (not part of the bot's interface).
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <string>

namespace nj::botu {

inline double dist(int ax, int ay, int bx, int by) { return std::hypot(double(ax - bx), double(ay - by)); }
inline int    pct(int64_t cur, int64_t max) { return max > 0 && cur >= 0 ? int(cur * 100 / max) : 100; }
inline long   num(const std::string& v) { return std::strtol(v.c_str(), nullptr, 10); }
inline bool   flag(const std::string& v) { return num(v) != 0; }

// "key.field=value"
template <typename T>
void kv(std::ostream& o, const char* key, const char* field, const T& v) { o << key << '.' << field << '=' << v << '\n'; }

inline bool shadow_monster(uint16_t species) { return species >= 16000 && species <= 16999; }

} // namespace nj::botu
