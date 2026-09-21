#include "config.h"

namespace sf {

// Every setting the driver understands. Used for DSN lookup.
static const char* kKnownKeys[] = {
    "AuthType", "LoginUrl", "InstanceUrl", "ClientId", "ClientSecret", "Username", "Password",
    "SecurityToken", "RefreshToken", "AccessToken", "PrivateKeyFile", "PrivateKeyPassword", "JwtAudience",
    "ApiVersion", "BatchSize", "Timeout", "MaxRetries", "VerifySSL", "CABundle", "ProxyUrl", "ProxyUser",
    "ProxyPassword", "MetadataCacheTTL", "LogFile", "LogLevel", "IncludeNonQueryable"};

std::string ConnConfig::canonical(const std::string& key) {
  std::string k = trim(key);
  if (iequals(k, "UID") || iequals(k, "User")) return "Username";
  if (iequals(k, "PWD")) return "Password";
  for (const char* known : kKnownKeys)
    if (iequals(k, known)) return known;
  return k;
}

bool ConnConfig::is_secret(const std::string& key) {
  static const char* secrets[] = {"Password", "ClientSecret", "SecurityToken", "RefreshToken",
                                  "AccessToken", "PrivateKeyPassword", "ProxyPassword", "PWD"};
  for (const char* s : secrets)
    if (iequals(key, s)) return true;
  return false;
}

ConnConfig ConnConfig::parse(const std::string& s) {
  ConnConfig c;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ';' || std::isspace(static_cast<unsigned char>(s[i])))) ++i;
    size_t eq = s.find('=', i);
    if (eq == std::string::npos) break;
    std::string key = trim(s.substr(i, eq - i));
    i = eq + 1;
    while (i < s.size() && s[i] == ' ') ++i;
    std::string value;
    if (i < s.size() && s[i] == '{') {
      ++i;
      while (i < s.size()) {
        if (s[i] == '}') {
          if (i + 1 < s.size() && s[i + 1] == '}') { value += '}'; i += 2; continue; }
          ++i;
          break;
        }
        value += s[i++];
      }
      while (i < s.size() && s[i] != ';') ++i;
    } else {
      size_t semi = s.find(';', i);
      value = trim(s.substr(i, semi == std::string::npos ? std::string::npos : semi - i));
      i = semi == std::string::npos ? s.size() : semi;
    }
    if (!key.empty()) c.kv_[canonical(key)] = value;
  }
  return c;
}

void ConnConfig::merge_dsn(const std::string& dsn) {
  if (dsn.empty()) return;
  char buf[4096];
  for (const char* key : kKnownKeys) {
    if (has(key)) continue;
    buf[0] = 0;
    int n = SQLGetPrivateProfileString(dsn.c_str(), key, "", buf, sizeof(buf), "odbc.ini");
    if (n > 0 && buf[0]) kv_[key] = buf;
  }
}

std::string ConnConfig::get(const std::string& key, const std::string& def) const {
  auto it = kv_.find(canonical(key));
  return it == kv_.end() || it->second.empty() ? def : it->second;
}
long long ConnConfig::get_int(const std::string& key, long long def) const {
  std::string v = get(key);
  if (v.empty()) return def;
  try { return std::stoll(v); } catch (...) {
    throw OdbcError("HY024", "Invalid numeric value for " + key + ": " + v);
  }
}
bool ConnConfig::get_bool(const std::string& key, bool def) const {
  std::string v = to_lower(get(key));
  if (v.empty()) return def;
  return v == "1" || v == "true" || v == "yes" || v == "on";
}
bool ConnConfig::has(const std::string& key) const {
  auto it = kv_.find(canonical(key));
  return it != kv_.end() && !it->second.empty();
}
void ConnConfig::set(const std::string& key, const std::string& value) { kv_[canonical(key)] = value; }

static std::string quote_value(const std::string& v) {
  if (v.find_first_of(";{}= ") == std::string::npos) return v;
  std::string out = "{";
  for (char c : v) { out += c; if (c == '}') out += '}'; }
  return out + "}";
}

std::string ConnConfig::to_connection_string() const {
  std::string out;
  for (auto& [k, v] : kv_) {
    if (iequals(k, "AccessToken")) continue;  // short-lived; never persist
    out += k + "=" + quote_value(v) + ";";
  }
  return out;
}

std::string ConnConfig::redacted() const {
  std::string out;
  for (auto& [k, v] : kv_) out += k + "=" + (is_secret(k) ? std::string("***") : v) + ";";
  return out;
}

}  // namespace sf
