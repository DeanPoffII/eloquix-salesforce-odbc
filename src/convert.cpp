#include "convert.h"

#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdio>

namespace sf {

SQLSMALLINT default_c_type(SQLSMALLINT sql_type) {
  switch (sql_type) {
    case SQL_CHAR: case SQL_VARCHAR: case SQL_LONGVARCHAR: return SQL_C_CHAR;
    case SQL_WCHAR: case SQL_WVARCHAR: case SQL_WLONGVARCHAR: return SQL_C_WCHAR;
    case SQL_BIT: return SQL_C_BIT;
    case SQL_TINYINT: return SQL_C_STINYINT;
    case SQL_SMALLINT: return SQL_C_SSHORT;
    case SQL_INTEGER: return SQL_C_SLONG;
    case SQL_BIGINT: return SQL_C_SBIGINT;
    case SQL_REAL: return SQL_C_FLOAT;
    case SQL_FLOAT: case SQL_DOUBLE: return SQL_C_DOUBLE;
    case SQL_DECIMAL: case SQL_NUMERIC: return SQL_C_CHAR;
    case SQL_TYPE_DATE: case SQL_DATE: return SQL_C_TYPE_DATE;
    case SQL_TYPE_TIME: case SQL_TIME: return SQL_C_TYPE_TIME;
    case SQL_TYPE_TIMESTAMP: case SQL_TIMESTAMP: return SQL_C_TYPE_TIMESTAMP;
    case SQL_BINARY: case SQL_VARBINARY: case SQL_LONGVARBINARY: return SQL_C_BINARY;
    default: return SQL_C_CHAR;
  }
}

namespace {

struct Num {
  double d = 0;
  bool ok = false;
};

Num parse_num(const std::string& raw) {
  Num n;
  std::string s = trim(raw);
  if (iequals(s, "true")) { n.d = 1; n.ok = true; return n; }
  if (iequals(s, "false")) { n.d = 0; n.ok = true; return n; }
  if (s.empty()) return n;
  char* end = nullptr;
  n.d = std::strtod(s.c_str(), &end);
  n.ok = end && *end == 0;
  return n;
}

// Exact 64-bit integer parse where possible (avoids double rounding for large ids/counts).
bool parse_ll(const std::string& raw, long long& out) {
  std::string s = trim(raw);
  if (s.empty() || s.find_first_of(".eE") != std::string::npos) return false;
  char* end = nullptr;
  errno = 0;
  out = std::strtoll(s.c_str(), &end, 10);
  return end && *end == 0 && errno == 0;
}

template <typename T>
SQLRETURN put_int(Diag& diag, const std::string& s, SQLPOINTER target, SQLLEN* ind, long double lo, long double hi) {
  long long ll;
  bool info = false;
  long double v;
  if (parse_ll(s, ll)) {
    v = static_cast<long double>(ll);
  } else {
    Num n = parse_num(s);
    if (!n.ok) { diag.add("22018", "Invalid character value for numeric conversion: " + s); return SQL_ERROR; }
    v = std::trunc(static_cast<long double>(n.d));
    info = v != static_cast<long double>(n.d);
  }
  if (v < lo || v > hi) { diag.add("22003", "Numeric value out of range: " + s); return SQL_ERROR; }
  if (target) {
    T t = static_cast<T>(v);
    std::memcpy(target, &t, sizeof(T));
  }
  if (ind) *ind = sizeof(T);
  if (info) { diag.add("01S07", "Fractional truncation"); return SQL_SUCCESS_WITH_INFO; }
  return SQL_SUCCESS;
}

SQLRETURN put_numeric(Diag& diag, const ColumnInfo& col, const std::string& raw, SQLPOINTER target, SQLLEN* ind) {
  std::string s = trim(raw);
  if (s.find_first_of("eE") != std::string::npos) {
    Num n = parse_num(s);
    if (!n.ok) { diag.add("22018", "Invalid numeric value: " + raw); return SQL_ERROR; }
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", std::max<int>(col.decimal_digits, 0), n.d);
    s = b;
  }
  SQL_NUMERIC_STRUCT ns{};
  ns.sign = 1;
  size_t i = 0;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) { ns.sign = s[i] == '-' ? 0 : 1; ++i; }
  std::string intpart, frac;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) intpart += s[i++];
  if (i < s.size() && s[i] == '.') { ++i; while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) frac += s[i++]; }
  if (i != s.size() || (intpart.empty() && frac.empty())) {
    diag.add("22018", "Invalid numeric value: " + raw);
    return SQL_ERROR;
  }
  int scale = std::clamp<int>(col.decimal_digits, 0, 38);
  bool truncated = false;
  if (static_cast<int>(frac.size()) > scale) {
    truncated = frac.find_first_not_of('0', scale) != std::string::npos;
    frac.resize(scale);
  }
  while (static_cast<int>(frac.size()) < scale) frac += '0';
  std::string digits = intpart + frac;
  for (char c : digits) {
    unsigned carry = static_cast<unsigned>(c - '0');
    for (int k = 0; k < SQL_MAX_NUMERIC_LEN; ++k) {
      unsigned v = ns.val[k] * 10u + carry;
      ns.val[k] = static_cast<SQLCHAR>(v & 0xFF);
      carry = v >> 8;
    }
    if (carry) { diag.add("22003", "Numeric value out of range: " + raw); return SQL_ERROR; }
  }
  ns.precision = static_cast<SQLCHAR>(std::clamp<SQLULEN>(col.column_size, 1, 38));
  ns.scale = static_cast<SQLSCHAR>(scale);
  if (target) std::memcpy(target, &ns, sizeof ns);
  if (ind) *ind = sizeof ns;
  if (truncated) { diag.add("01S07", "Fractional truncation"); return SQL_SUCCESS_WITH_INFO; }
  return SQL_SUCCESS;
}

}  // namespace

SQLLEN c_type_size(SQLSMALLINT c) {
  switch (c) {
    case SQL_C_SHORT: case SQL_C_SSHORT: case SQL_C_USHORT: return sizeof(SQLSMALLINT);
    case SQL_C_LONG: case SQL_C_SLONG: case SQL_C_ULONG: return sizeof(SQLINTEGER);
    case SQL_C_SBIGINT: case SQL_C_UBIGINT: return sizeof(SQLBIGINT);
    case SQL_C_TINYINT: case SQL_C_STINYINT: case SQL_C_UTINYINT: case SQL_C_BIT: return 1;
    case SQL_C_FLOAT: return sizeof(SQLREAL);
    case SQL_C_DOUBLE: return sizeof(SQLDOUBLE);
    case SQL_C_TYPE_DATE: case SQL_C_DATE: return sizeof(SQL_DATE_STRUCT);
    case SQL_C_TYPE_TIME: case SQL_C_TIME: return sizeof(SQL_TIME_STRUCT);
    case SQL_C_TYPE_TIMESTAMP: case SQL_C_TIMESTAMP: return sizeof(SQL_TIMESTAMP_STRUCT);
    case SQL_C_NUMERIC: return sizeof(SQL_NUMERIC_STRUCT);
    default: return 0;
  }
}

SQLRETURN get_value(Diag& diag, const ColumnInfo& col, const Value& v, SQLSMALLINT c_type, SQLPOINTER target,
                    SQLLEN buffer_len, SQLLEN* ind, GetDataState* state) {
  if (c_type == SQL_C_DEFAULT) c_type = default_c_type(col.sql_type);
  if (state && state->done) return SQL_NO_DATA;

  if (!v) {
    if (!ind) {
      diag.add("22002", "Indicator variable required but not supplied");
      return SQL_ERROR;
    }
    *ind = SQL_NULL_DATA;
    if (state) state->done = true;
    return SQL_SUCCESS;
  }
  const std::string& s = *v;

  switch (c_type) {
    case SQL_C_CHAR:
    case SQL_C_BINARY: {
      bool is_char = c_type == SQL_C_CHAR;
      SQLLEN offset = state ? state->offset : 0;
      SQLLEN remaining = static_cast<SQLLEN>(s.size()) - offset;
      if (ind) *ind = remaining;
      SQLLEN avail = is_char ? buffer_len - 1 : buffer_len;
      if (!target || avail < 0) {
        if (remaining == 0 && !is_char) { if (state) state->done = true; return SQL_SUCCESS; }
        diag.add("01004", "String data, right truncated");
        return SQL_SUCCESS_WITH_INFO;
      }
      SQLLEN n = std::min(remaining, avail);
      if (is_char && n < remaining && n > 0) {
        SQLLEN k = n;
        while (k > 0 && (static_cast<unsigned char>(s[offset + k]) & 0xC0) == 0x80) --k;
        if (k > 0) n = k;
      }
      std::memcpy(target, s.data() + offset, static_cast<size_t>(n));
      if (is_char) static_cast<char*>(target)[n] = 0;
      if (n < remaining) {
        if (state) state->offset += n;
        diag.add("01004", "String data, right truncated");
        return SQL_SUCCESS_WITH_INFO;
      }
      if (state) state->done = true;
      return SQL_SUCCESS;
    }
    case SQL_C_WCHAR: {
      auto w = utf8_to_wide(s);
      const SQLLEN cw = sizeof(SQLWCHAR);
      SQLLEN offset = state ? state->offset : 0;
      SQLLEN remaining = static_cast<SQLLEN>(w.size()) - offset;
      if (ind) *ind = remaining * cw;
      SQLLEN avail = buffer_len / cw - 1;
      if (!target || avail < 0) {
        diag.add("01004", "String data, right truncated");
        return SQL_SUCCESS_WITH_INFO;
      }
      SQLLEN n = std::min(remaining, avail);
      if (cw == 2 && n > 0 && n < remaining) {
        uint32_t last = static_cast<uint32_t>(w[offset + n - 1]);
        if (last >= 0xD800 && last <= 0xDBFF && n > 1) --n;
      }
      std::memcpy(target, w.data() + offset, static_cast<size_t>(n * cw));
      static_cast<SQLWCHAR*>(target)[n] = 0;
      if (n < remaining) {
        if (state) state->offset += n;
        diag.add("01004", "String data, right truncated");
        return SQL_SUCCESS_WITH_INFO;
      }
      if (state) state->done = true;
      return SQL_SUCCESS;
    }
    default:
      break;
  }

  SQLRETURN rc;
  switch (c_type) {
    case SQL_C_SHORT: case SQL_C_SSHORT: rc = put_int<SQLSMALLINT>(diag, s, target, ind, SHRT_MIN, SHRT_MAX); break;
    case SQL_C_USHORT: rc = put_int<SQLUSMALLINT>(diag, s, target, ind, 0, USHRT_MAX); break;
    case SQL_C_LONG: case SQL_C_SLONG: rc = put_int<SQLINTEGER>(diag, s, target, ind, INT_MIN, INT_MAX); break;
    case SQL_C_ULONG: rc = put_int<SQLUINTEGER>(diag, s, target, ind, 0, UINT_MAX); break;
    case SQL_C_SBIGINT: rc = put_int<SQLBIGINT>(diag, s, target, ind, (long double)LLONG_MIN, (long double)LLONG_MAX); break;
    case SQL_C_UBIGINT: rc = put_int<SQLUBIGINT>(diag, s, target, ind, 0, (long double)ULLONG_MAX); break;
    case SQL_C_TINYINT: case SQL_C_STINYINT: rc = put_int<SQLSCHAR>(diag, s, target, ind, SCHAR_MIN, SCHAR_MAX); break;
    case SQL_C_UTINYINT: rc = put_int<SQLCHAR>(diag, s, target, ind, 0, UCHAR_MAX); break;
    case SQL_C_BIT: rc = put_int<SQLCHAR>(diag, s, target, ind, 0, 1); break;
    case SQL_C_FLOAT:
    case SQL_C_DOUBLE: {
      Num n = parse_num(s);
      if (!n.ok) { diag.add("22018", "Invalid character value for numeric conversion: " + s); return SQL_ERROR; }
      if (c_type == SQL_C_FLOAT) {
        if (std::fabs(n.d) > FLT_MAX) { diag.add("22003", "Numeric value out of range"); return SQL_ERROR; }
        SQLREAL f = static_cast<SQLREAL>(n.d);
        if (target) std::memcpy(target, &f, sizeof f);
        if (ind) *ind = sizeof f;
      } else {
        if (target) std::memcpy(target, &n.d, sizeof n.d);
        if (ind) *ind = sizeof n.d;
      }
      rc = SQL_SUCCESS;
      break;
    }
    case SQL_C_NUMERIC:
      rc = put_numeric(diag, col, s, target, ind);
      break;
    case SQL_C_TYPE_DATE: case SQL_C_DATE:
    case SQL_C_TYPE_TIME: case SQL_C_TIME:
    case SQL_C_TYPE_TIMESTAMP: case SQL_C_TIMESTAMP: {
      DateTimeParts p;
      if (!parse_datetime(s, p)) { diag.add("22018", "Invalid datetime value: " + s); return SQL_ERROR; }
      rc = SQL_SUCCESS;
      if (c_type == SQL_C_TYPE_DATE || c_type == SQL_C_DATE) {
        if (!p.has_date) { diag.add("07006", "Restricted data type attribute violation"); return SQL_ERROR; }
        SQL_DATE_STRUCT d{static_cast<SQLSMALLINT>(p.year), static_cast<SQLUSMALLINT>(p.month),
                          static_cast<SQLUSMALLINT>(p.day)};
        if (target) std::memcpy(target, &d, sizeof d);
        if (ind) *ind = sizeof d;
        if (p.has_time && (p.hour || p.minute || p.second || p.millis)) {
          diag.add("01S07", "Fractional truncation");
          rc = SQL_SUCCESS_WITH_INFO;
        }
      } else if (c_type == SQL_C_TYPE_TIME || c_type == SQL_C_TIME) {
        if (!p.has_time) { diag.add("07006", "Restricted data type attribute violation"); return SQL_ERROR; }
        SQL_TIME_STRUCT t{static_cast<SQLUSMALLINT>(p.hour), static_cast<SQLUSMALLINT>(p.minute),
                          static_cast<SQLUSMALLINT>(p.second)};
        if (target) std::memcpy(target, &t, sizeof t);
        if (ind) *ind = sizeof t;
      } else {
        if (!p.has_date) { diag.add("07006", "Restricted data type attribute violation"); return SQL_ERROR; }
        SQL_TIMESTAMP_STRUCT ts{};
        ts.year = static_cast<SQLSMALLINT>(p.year);
        ts.month = static_cast<SQLUSMALLINT>(p.month);
        ts.day = static_cast<SQLUSMALLINT>(p.day);
        ts.hour = static_cast<SQLUSMALLINT>(p.hour);
        ts.minute = static_cast<SQLUSMALLINT>(p.minute);
        ts.second = static_cast<SQLUSMALLINT>(p.second);
        ts.fraction = static_cast<SQLUINTEGER>(p.millis) * 1000000u;
        if (target) std::memcpy(target, &ts, sizeof ts);
        if (ind) *ind = sizeof ts;
      }
      break;
    }
    default:
      diag.add("07006", "Conversion to C type " + std::to_string(c_type) + " is not supported");
      return SQL_ERROR;
  }
  if (rc != SQL_ERROR && state) state->done = true;
  return rc;
}

// ------------------------------------------------------------ parameters

Lit read_param(const ParamBinding& b, SQLLEN off) {
  if (!b.bound) throw OdbcError("07002", "Not all parameters are bound");
  Lit l;
  SQLLEN* ind = b.str_len_or_ind ? reinterpret_cast<SQLLEN*>(reinterpret_cast<char*>(b.str_len_or_ind) + off)
                                 : nullptr;
  if (ind && *ind == SQL_NULL_DATA) return l;
  if (ind && (*ind == SQL_DATA_AT_EXEC || *ind <= SQL_LEN_DATA_AT_EXEC_OFFSET))
    throw OdbcError("HYC00", "Data-at-execution parameters are not supported");
  if (!b.value) return l;
  const char* p = static_cast<const char*>(b.value) + off;
  SQLSMALLINT c = b.c_type == SQL_C_DEFAULT ? default_c_type(b.sql_type) : b.c_type;
  char buf[64];
  auto num = [&](const std::string& s) { l.kind = Lit::Number; l.text = s; };

  switch (c) {
    case SQL_C_CHAR:
    case SQL_C_BINARY: {
      SQLLEN len = ind ? *ind : SQL_NTS;
      l.kind = Lit::String;
      l.text = len == SQL_NTS ? std::string(p) : std::string(p, static_cast<size_t>(len));
      break;
    }
    case SQL_C_WCHAR: {
      SQLLEN len = ind ? *ind : SQL_NTS;
      l.kind = Lit::String;
      l.text = wide_in(reinterpret_cast<const SQLWCHAR*>(p),
                       len == SQL_NTS ? SQL_NTS : len / static_cast<SQLLEN>(sizeof(SQLWCHAR)));
      break;
    }
    case SQL_C_SHORT: case SQL_C_SSHORT: { SQLSMALLINT v; std::memcpy(&v, p, sizeof v); num(std::to_string(v)); break; }
    case SQL_C_USHORT: { SQLUSMALLINT v; std::memcpy(&v, p, sizeof v); num(std::to_string(v)); break; }
    case SQL_C_LONG: case SQL_C_SLONG: { SQLINTEGER v; std::memcpy(&v, p, sizeof v); num(std::to_string(v)); break; }
    case SQL_C_ULONG: { SQLUINTEGER v; std::memcpy(&v, p, sizeof v); num(std::to_string(v)); break; }
    case SQL_C_SBIGINT: { SQLBIGINT v; std::memcpy(&v, p, sizeof v); num(std::to_string(v)); break; }
    case SQL_C_UBIGINT: { SQLUBIGINT v; std::memcpy(&v, p, sizeof v); num(std::to_string(v)); break; }
    case SQL_C_TINYINT: case SQL_C_STINYINT: num(std::to_string(static_cast<int>(static_cast<signed char>(*p)))); break;
    case SQL_C_UTINYINT: num(std::to_string(static_cast<int>(static_cast<unsigned char>(*p)))); break;
    case SQL_C_BIT: l.kind = Lit::Bool; l.text = *p ? "true" : "false"; break;
    case SQL_C_FLOAT: { SQLREAL v; std::memcpy(&v, p, sizeof v); std::snprintf(buf, sizeof buf, "%.9g", v); num(buf); break; }
    case SQL_C_DOUBLE: { SQLDOUBLE v; std::memcpy(&v, p, sizeof v); std::snprintf(buf, sizeof buf, "%.17g", v); num(buf); break; }
    case SQL_C_TYPE_DATE: case SQL_C_DATE: {
      SQL_DATE_STRUCT d; std::memcpy(&d, p, sizeof d);
      std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", d.year, d.month, d.day);
      l.kind = Lit::Date; l.text = buf; break;
    }
    case SQL_C_TYPE_TIME: case SQL_C_TIME: {
      SQL_TIME_STRUCT t; std::memcpy(&t, p, sizeof t);
      std::snprintf(buf, sizeof buf, "%02u:%02u:%02u", t.hour, t.minute, t.second);
      l.kind = Lit::Time; l.text = buf; break;
    }
    case SQL_C_TYPE_TIMESTAMP: case SQL_C_TIMESTAMP: {
      SQL_TIMESTAMP_STRUCT t; std::memcpy(&t, p, sizeof t);
      std::snprintf(buf, sizeof buf, "%04d-%02u-%02u %02u:%02u:%02u.%03u", t.year, t.month, t.day, t.hour,
                    t.minute, t.second, static_cast<unsigned>(t.fraction / 1000000u));
      l.kind = Lit::Timestamp; l.text = buf; break;
    }
    case SQL_C_NUMERIC: {
      SQL_NUMERIC_STRUCT ns; std::memcpy(&ns, p, sizeof ns);
      unsigned char val[SQL_MAX_NUMERIC_LEN];
      std::memcpy(val, ns.val, sizeof val);
      std::string digits;
      bool nonzero = true;
      while (nonzero) {  // repeated division by 10 of a little-endian 128-bit integer
        unsigned rem = 0;
        nonzero = false;
        for (int k = SQL_MAX_NUMERIC_LEN - 1; k >= 0; --k) {
          unsigned cur = (rem << 8) | val[k];
          val[k] = static_cast<unsigned char>(cur / 10);
          rem = cur % 10;
          if (val[k]) nonzero = true;
        }
        digits.insert(digits.begin(), static_cast<char>('0' + rem));
      }
      int scale = ns.scale;
      if (scale > 0) {
        while (static_cast<int>(digits.size()) <= scale) digits.insert(digits.begin(), '0');
        digits.insert(digits.size() - scale, ".");
      } else if (scale < 0) {
        digits.append(static_cast<size_t>(-scale), '0');
      }
      num((ns.sign ? "" : "-") + digits);
      break;
    }
    default:
      throw OdbcError("HYC00", "Parameter C type " + std::to_string(c) + " is not supported");
  }
  return l;
}

}  // namespace sf
