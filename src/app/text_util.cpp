#include "app/text_util.h"
#include <windows.h>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace nj {

static std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) ++a;
    while (b > a && isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string fmt(const char* f, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    if (n < 0) return {};
    return std::string(buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
}

std::string cp949_to_utf8(const char* s, size_t n)
{
    if (!n) return {};
    int wn = MultiByteToWideChar(949, 0, s, (int)n, nullptr, 0);
    if (wn <= 0) return std::string(s, n);
    std::wstring w(wn, L'\0');
    MultiByteToWideChar(949, 0, s, (int)n, &w[0], wn);
    int un = WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
    std::string u(un, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, &u[0], un, nullptr, nullptr);
    return u;
}

std::string utf8_to_cp949(const std::string& s)
{
    if (s.empty()) return {};
    int wn = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (wn <= 0) return s;
    std::wstring w(wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], wn);
    int cn = WideCharToMultiByte(949, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
    std::string c(cn, '\0');
    WideCharToMultiByte(949, 0, w.data(), wn, &c[0], cn, nullptr, nullptr);
    return c;
}

std::string hex_preview(const uint8_t* p, size_t n, size_t max)
{
    static const char* digits = "0123456789ABCDEF";
    size_t m = n < max ? n : max;
    std::string s;
    s.reserve(m * 3 + 4);
    for (size_t i = 0; i < m; ++i) {
        if (i) s += ' ';
        s += digits[p[i] >> 4];
        s += digits[p[i] & 15];
    }
    if (m < n) s += " ...";
    return s;
}

bool parse_hex(const std::string& in, std::vector<uint8_t>& out, std::string& err)
{
    out.clear();
    int hi = -1;
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '0' && i + 1 < in.size() && (in[i + 1] == 'x' || in[i + 1] == 'X') && hi < 0) {
            ++i;
            continue;
        }
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else if (isspace((unsigned char)c) || c == ',' || c == '-' || c == ':') {
            if (hi >= 0) { err = fmt("odd hex digit before position %zu", i); return false; }
            continue;
        } else {
            err = fmt("not hex: '%c' at position %zu", c, i);
            return false;
        }
        if (hi < 0) hi = v;
        else { out.push_back(uint8_t(hi << 4 | v)); hi = -1; }
    }
    if (hi >= 0) { err = "odd number of hex digits"; return false; }
    return true;
}

bool parse_uint(const std::string& s, uint32_t& v)
{
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) ++a;
    while (b > a && isspace((unsigned char)s[b - 1])) --b;
    if (a == b) return false;
    std::string t = s.substr(a, b - a);
    char* end = nullptr;
    unsigned long r;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) r = strtoul(t.c_str() + 2, &end, 16);
    else r = strtoul(t.c_str(), &end, 10);
    if (!end || *end) return false;
    v = (uint32_t)r;
    return true;
}

bool parse_uint64(const std::string& s, uint64_t& v)
{
    std::string t = trim(s);
    if (t.empty()) return false;
    char* end = nullptr;
    unsigned long long r;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) r = strtoull(t.c_str() + 2, &end, 16);
    else r = strtoull(t.c_str(), &end, 10);
    if (!end || *end) return false;
    v = (uint64_t)r;
    return true;
}

bool parse_int64(const std::string& s, int64_t& v)
{
    std::string t = trim(s);
    if (t.empty()) return false;
    bool neg = t[0] == '-';
    if (neg || t[0] == '+') t = t.substr(1);
    char* end = nullptr;
    long long r;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) r = (long long)strtoull(t.c_str() + 2, &end, 16);
    else r = strtoll(t.c_str(), &end, 10);
    if (!end || *end) return false;
    v = neg ? -r : r;
    return true;
}

uint64_t now_filetime()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::string filetime_local_hms(uint64_t ft)
{
    FILETIME u{ (DWORD)ft, (DWORD)(ft >> 32) }, l;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&u, &l) || !FileTimeToSystemTime(&l, &st)) return "?";
    return fmt("%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

std::string filetime_local_date(uint64_t ft)
{
    FILETIME u{ (DWORD)ft, (DWORD)(ft >> 32) }, l;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&u, &l) || !FileTimeToSystemTime(&l, &st)) return "?";
    return fmt("%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
}

std::string exe_dir()
{
    char p[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, p, MAX_PATH);
    char* s = strrchr(p, '\\');
    if (s) s[1] = '\0';
    return p;
}

bool file_exists(const std::string& path)
{
    DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

namespace {
std::mutex              g_logMu;
std::deque<std::string> g_log;
uint64_t                g_logVersion = 0;
constexpr size_t        kMaxLog = 5000;
}

void app_log(const char* f, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    std::string line = fmt("%02u:%02u:%02u  %s", st.wHour, st.wMinute, st.wSecond, buf);
    std::lock_guard<std::mutex> g(g_logMu);
    g_log.push_back(std::move(line));
    while (g_log.size() > kMaxLog) g_log.pop_front();
    ++g_logVersion;
}

void app_log_read(const std::function<void(const std::deque<std::string>&)>& fn)
{
    std::lock_guard<std::mutex> g(g_logMu);
    fn(g_log);
}

void app_log_clear()
{
    std::lock_guard<std::mutex> g(g_logMu);
    g_log.clear();
    ++g_logVersion;
}

uint64_t app_log_version()
{
    std::lock_guard<std::mutex> g(g_logMu);
    return g_logVersion;
}

} // namespace nj
