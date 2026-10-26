#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace nj {

std::string fmt(const char* f, ...);

// The client's text is CP949 (Korean); ImGui wants UTF-8.
std::string cp949_to_utf8(const char* s, size_t n);
std::string utf8_to_cp949(const std::string& s);    // for building a packet's string fields

// "AA BB CC" (at most `max` bytes, "..." when cut).
std::string hex_preview(const uint8_t* p, size_t n, size_t max);
// Accepts "AA BB", "aabb", "0xAA,0xBB", newlines; anything else is an error.
bool parse_hex(const std::string& in, std::vector<uint8_t>& out, std::string& err);
// Decimal or 0x-hex.
bool parse_uint(const std::string& s, uint32_t& v);
bool parse_uint64(const std::string& s, uint64_t& v);
bool parse_int64(const std::string& s, int64_t& v);   // signed, decimal or 0x-hex

// The current UTC FILETIME (the packets' timestamps).
uint64_t now_filetime();
// UTC FILETIME -> local "HH:MM:SS.mmm".
std::string filetime_local_hms(uint64_t ft);
std::string filetime_local_date(uint64_t ft);       // "2026-10-23 21:05"

std::string exe_dir();   // directory of TricksterAutomationStudio.exe, with a trailing backslash
bool file_exists(const std::string& path);

// Application log (thread-safe).
void app_log(const char* f, ...);
void app_log_read(const std::function<void(const std::deque<std::string>&)>& fn);
void app_log_clear();
uint64_t app_log_version();

} // namespace nj
