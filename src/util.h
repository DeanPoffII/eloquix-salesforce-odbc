#pragma once
#include "common.h"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <sstream>

namespace sf {

// ---------- ASCII helpers ----------
std::string to_upper(std::string s);
std::string to_lower(std::string s);
bool iequals(const std::string& a, const std::string& b);
std::string trim(const std::string& s);
bool starts_with_ci(const std::string& s, const std::string& prefix);

struct CiLess {
  bool operator()(const std::string& a, const std::string& b) const;
};

// ---------- Encoding ----------
// Input strings from ANSI entry points are treated as UTF-8.
std::string narrow_in(const SQLCHAR* s, SQLLEN len);
std::string wide_in(const SQLWCHAR* s, SQLLEN len_chars);
std::basic_string<SQLWCHAR> utf8_to_wide(const std::string& s);
std::string wide_to_utf8(const SQLWCHAR* s, size_t n);

// Output helpers. Return true when the value was truncated.
bool put_utf8(const std::string& v, SQLPOINTER buf, SQLLEN cap_bytes, SQLLEN* out_len);
bool put_wide(const std::string& v, SQLPOINTER buf, SQLLEN cap_chars, SQLLEN* out_len_chars);

template <typename L>
bool out_a(const std::string& v, SQLPOINTER buf, SQLLEN cap_bytes, L* out) {
  SQLLEN n = 0;
  bool t = put_utf8(v, buf, cap_bytes, &n);
  if (out) *out = static_cast<L>(n);
  return t;
}
// cap and reported length in characters
template <typename L>
bool out_w_chars(const std::string& v, SQLPOINTER buf, SQLLEN cap_chars, L* out) {
  SQLLEN n = 0;
  bool t = put_wide(v, buf, cap_chars, &n);
  if (out) *out = static_cast<L>(n);
  return t;
}
// cap and reported length in bytes
template <typename L>
bool out_w_bytes(const std::string& v, SQLPOINTER buf, SQLLEN cap_bytes, L* out) {
  SQLLEN n = 0;
  bool t = put_wide(v, buf, cap_bytes / static_cast<SQLLEN>(sizeof(SQLWCHAR)), &n);
  if (out) *out = static_cast<L>(n * sizeof(SQLWCHAR));
  return t;
}

// ---------- Patterns ----------
// ODBC search pattern: % and _ wildcards, backslash escape, case-insensitive.
bool like_match(const std::string& pattern, const std::string& value);
bool is_all_pattern(const std::string& pattern);  // "%" only

// ---------- Dates ----------
struct DateTimeParts {
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  int millis = 0;
  bool has_date = false, has_time = false;
};
// Accepts "YYYY-MM-DD", "HH:MM:SS[.fff]", "YYYY-MM-DD[T ]HH:MM:SS[.fff][Z|+HHMM|+HH:MM]".
// Offsets are normalised to UTC.
bool parse_datetime(const std::string& s, DateTimeParts& out);
std::string format_date(const DateTimeParts& p);       // YYYY-MM-DD
std::string format_time(const DateTimeParts& p);       // HH:MM:SS
std::string format_timestamp(const DateTimeParts& p);  // YYYY-MM-DD HH:MM:SS.fff

// ---------- Encoding helpers for HTTP/JWT ----------
std::string base64url(const std::string& data);
std::string url_encode(const std::string& s);

// ---------- Logging ----------
class Logger {
 public:
  static Logger& instance();
  void configure(const std::string& path, int level);
  int level() const { return level_; }
  void write(int level, const std::string& msg);

 private:
  std::mutex mu_;
  std::ofstream out_;
  int level_ = 0;
  std::string path_;
};

#define SF_LOG(lvl, expr)                                         \
  do {                                                            \
    if (::sf::Logger::instance().level() >= (lvl)) {              \
      std::ostringstream _sf_os;                                  \
      _sf_os << expr;                                             \
      ::sf::Logger::instance().write((lvl), _sf_os.str());        \
    }                                                             \
  } while (0)
#define LOG_ERROR(expr) SF_LOG(1, expr)
#define LOG_INFO(expr) SF_LOG(2, expr)
#define LOG_DEBUG(expr) SF_LOG(3, expr)

}  // namespace sf
