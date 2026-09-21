#include "util.h"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace sf {

std::string to_upper(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}
std::string to_lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
bool iequals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}
bool CiLess::operator()(const std::string& a, const std::string& b) const {
  return to_lower(a) < to_lower(b);
}
std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}
bool starts_with_ci(const std::string& s, const std::string& p) {
  return s.size() >= p.size() && iequals(s.substr(0, p.size()), p);
}

// ---------------- UTF-8 / UTF-16 / UTF-32 ----------------

static void append_utf8(std::string& out, uint32_t cp) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

std::basic_string<SQLWCHAR> utf8_to_wide(const std::string& s) {
  std::basic_string<SQLWCHAR> out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp;
    size_t extra;
    if (c < 0x80) { cp = c; extra = 0; }
    else if ((c >> 5) == 0x6) { cp = c & 0x1F; extra = 1; }
    else if ((c >> 4) == 0xE) { cp = c & 0x0F; extra = 2; }
    else if ((c >> 3) == 0x1E) { cp = c & 0x07; extra = 3; }
    else { cp = 0xFFFD; extra = 0; }
    bool ok = i + extra < s.size() || extra == 0;
    for (size_t k = 1; ok && k <= extra; ++k) {
      unsigned char cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) { ok = false; break; }
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (!ok) { cp = 0xFFFD; extra = 0; }
    i += extra + 1;
    if (sizeof(SQLWCHAR) == 2) {
      if (cp >= 0x10000 && cp <= 0x10FFFF) {
        cp -= 0x10000;
        out.push_back(static_cast<SQLWCHAR>(0xD800 + (cp >> 10)));
        out.push_back(static_cast<SQLWCHAR>(0xDC00 + (cp & 0x3FF)));
      } else {
        out.push_back(static_cast<SQLWCHAR>(cp));
      }
    } else {
      out.push_back(static_cast<SQLWCHAR>(cp));
    }
  }
  return out;
}

std::string wide_to_utf8(const SQLWCHAR* s, size_t n) {
  std::string out;
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    uint32_t cp = static_cast<uint32_t>(s[i]);
    if (sizeof(SQLWCHAR) == 2 && cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n) {
      uint32_t lo = static_cast<uint32_t>(s[i + 1]);
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        ++i;
      }
    }
    append_utf8(out, cp);
  }
  return out;
}

std::string narrow_in(const SQLCHAR* s, SQLLEN len) {
  if (!s) return "";
  if (len == SQL_NTS) return std::string(reinterpret_cast<const char*>(s));
  if (len < 0) return "";
  return std::string(reinterpret_cast<const char*>(s), static_cast<size_t>(len));
}

std::string wide_in(const SQLWCHAR* s, SQLLEN len) {
  if (!s) return "";
  size_t n;
  if (len == SQL_NTS) {
    n = 0;
    while (s[n]) ++n;
  } else if (len < 0) {
    return "";
  } else {
    n = static_cast<size_t>(len);
  }
  return wide_to_utf8(s, n);
}

bool put_utf8(const std::string& v, SQLPOINTER buf, SQLLEN cap, SQLLEN* out_len) {
  if (out_len) *out_len = static_cast<SQLLEN>(v.size());
  if (!buf) return false;
  if (cap <= 0) return !v.empty();
  size_t n = std::min<size_t>(v.size(), static_cast<size_t>(cap - 1));
  if (n < v.size()) {
    size_t k = n;  // never split a multi-byte sequence
    while (k > 0 && (static_cast<unsigned char>(v[k]) & 0xC0) == 0x80) --k;
    n = k;
  }
  std::memcpy(buf, v.data(), n);
  static_cast<char*>(buf)[n] = 0;
  return n < v.size();
}

bool put_wide(const std::string& v, SQLPOINTER buf, SQLLEN cap_chars, SQLLEN* out_len) {
  auto w = utf8_to_wide(v);
  if (out_len) *out_len = static_cast<SQLLEN>(w.size());
  if (!buf) return false;
  if (cap_chars <= 0) return !w.empty();
  size_t n = std::min<size_t>(w.size(), static_cast<size_t>(cap_chars - 1));
  if (sizeof(SQLWCHAR) == 2 && n > 0 && n < w.size()) {
    uint32_t last = static_cast<uint32_t>(w[n - 1]);
    if (last >= 0xD800 && last <= 0xDBFF) --n;
  }
  std::memcpy(buf, w.data(), n * sizeof(SQLWCHAR));
  static_cast<SQLWCHAR*>(buf)[n] = 0;
  return n < w.size();
}

// ---------------- Patterns ----------------

static bool like_impl(const std::string& p, size_t pi, const std::string& v, size_t vi) {
  while (pi < p.size()) {
    char c = p[pi];
    if (c == '\\' && pi + 1 < p.size()) {
      if (vi >= v.size() || std::tolower((unsigned char)p[pi + 1]) != std::tolower((unsigned char)v[vi]))
        return false;
      pi += 2; ++vi;
    } else if (c == '%') {
      while (pi < p.size() && p[pi] == '%') ++pi;
      if (pi == p.size()) return true;
      for (size_t k = vi; k <= v.size(); ++k)
        if (like_impl(p, pi, v, k)) return true;
      return false;
    } else if (c == '_') {
      if (vi >= v.size()) return false;
      ++pi; ++vi;
    } else {
      if (vi >= v.size() || std::tolower((unsigned char)c) != std::tolower((unsigned char)v[vi])) return false;
      ++pi; ++vi;
    }
  }
  return vi == v.size();
}

bool like_match(const std::string& pattern, const std::string& value) {
  return like_impl(pattern, 0, value, 0);
}
bool is_all_pattern(const std::string& p) { return p == "%"; }

// ---------------- Dates ----------------

static long long days_from_civil(long long y, unsigned m, unsigned d) {
  y -= m <= 2;
  const long long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<long long>(doe) - 719468;
}
static void civil_from_days(long long z, int& y, int& m, int& d) {
  z += 719468;
  const long long era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const long long yy = static_cast<long long>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int>(yy + (m <= 2));
}

static bool read_int(const std::string& s, size_t& i, int digits, int& out) {
  if (i + digits > s.size()) return false;
  int v = 0;
  for (int k = 0; k < digits; ++k) {
    char c = s[i + k];
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  out = v;
  i += digits;
  return true;
}

bool parse_datetime(const std::string& in, DateTimeParts& p) {
  std::string s = trim(in);
  p = DateTimeParts{};
  size_t i = 0;
  if (s.size() >= 10 && s[4] == '-' && s[7] == '-') {
    if (!read_int(s, i, 4, p.year) || s[i++] != '-' || !read_int(s, i, 2, p.month) || s[i++] != '-' ||
        !read_int(s, i, 2, p.day))
      return false;
    if (p.month < 1 || p.month > 12 || p.day < 1 || p.day > 31) return false;
    p.has_date = true;
    if (i < s.size() && (s[i] == 'T' || s[i] == ' ')) ++i;
  }
  if (i < s.size()) {
    if (!read_int(s, i, 2, p.hour) || i >= s.size() || s[i++] != ':' || !read_int(s, i, 2, p.minute))
      return false;
    if (i < s.size() && s[i] == ':') {
      ++i;
      if (!read_int(s, i, 2, p.second)) return false;
    }
    if (i < s.size() && s[i] == '.') {
      ++i;
      int ms = 0, digits = 0;
      while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        if (digits < 3) { ms = ms * 10 + (s[i] - '0'); ++digits; }
        ++i;
      }
      while (digits < 3) { ms *= 10; ++digits; }
      p.millis = ms;
    }
    if (p.hour > 23 || p.minute > 59 || p.second > 60) return false;
    p.has_time = true;
    int offset_min = 0;
    if (i < s.size() && (s[i] == 'Z' || s[i] == 'z')) {
      ++i;
    } else if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
      int sign = s[i] == '-' ? -1 : 1;
      ++i;
      int oh = 0, om = 0;
      if (!read_int(s, i, 2, oh)) return false;
      if (i < s.size() && s[i] == ':') ++i;
      if (i < s.size() && !read_int(s, i, 2, om)) return false;
      offset_min = sign * (oh * 60 + om);
    }
    if (i != s.size()) return false;
    if (offset_min != 0 && p.has_date) {
      long long total = days_from_civil(p.year, p.month, p.day) * 1440 + p.hour * 60 + p.minute - offset_min;
      long long days = total >= 0 ? total / 1440 : (total - 1439) / 1440;
      long long mins = total - days * 1440;
      civil_from_days(days, p.year, p.month, p.day);
      p.hour = static_cast<int>(mins / 60);
      p.minute = static_cast<int>(mins % 60);
    }
  }
  return p.has_date || p.has_time;
}

std::string format_date(const DateTimeParts& p) {
  char b[16];
  std::snprintf(b, sizeof b, "%04d-%02d-%02d", p.year, p.month, p.day);
  return b;
}
std::string format_time(const DateTimeParts& p) {
  char b[16];
  std::snprintf(b, sizeof b, "%02d:%02d:%02d", p.hour, p.minute, p.second);
  return b;
}
std::string format_timestamp(const DateTimeParts& p) {
  char b[32];
  std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d.%03d", p.year, p.month, p.day, p.hour, p.minute,
                p.second, p.millis);
  return b;
}

// ---------------- Encoding ----------------

std::string base64url(const std::string& data) {
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  size_t i = 0;
  while (i + 2 < data.size()) {
    uint32_t n = (uint8_t)data[i] << 16 | (uint8_t)data[i + 1] << 8 | (uint8_t)data[i + 2];
    out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63]; out += tbl[(n >> 6) & 63]; out += tbl[n & 63];
    i += 3;
  }
  if (data.size() - i == 1) {
    uint32_t n = (uint8_t)data[i] << 16;
    out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63];
  } else if (data.size() - i == 2) {
    uint32_t n = (uint8_t)data[i] << 16 | (uint8_t)data[i + 1] << 8;
    out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63]; out += tbl[(n >> 6) & 63];
  }
  return out;
}

std::string url_encode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%'; out += hex[c >> 4]; out += hex[c & 15];
    }
  }
  return out;
}

// ---------------- Logger ----------------

Logger& Logger::instance() {
  static Logger l;
  return l;
}

void Logger::configure(const std::string& path, int level) {
  std::lock_guard<std::mutex> g(mu_);
  if (path.empty() || level <= 0) return;
  if (path != path_) {
    if (out_.is_open()) out_.close();
    out_.open(path, std::ios::app);
    path_ = path;
  }
  level_ = std::max(level_, level);
}

void Logger::write(int level, const std::string& msg) {
  std::lock_guard<std::mutex> g(mu_);
  if (!out_.is_open()) return;
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  char ts[32];
  std::tm tmv{};
#ifdef _WIN32
  gmtime_s(&tmv, &t);
#else
  gmtime_r(&t, &tmv);
#endif
  std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tmv);
  static const char* names[] = {"", "ERROR", "INFO", "DEBUG"};
  out_ << ts << " " << names[std::min(level, 3)] << " " << msg << "\n";
  out_.flush();
}

}  // namespace sf
